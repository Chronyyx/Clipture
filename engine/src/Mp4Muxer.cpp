#include "clipture/Mp4Muxer.hpp"
#include "clipture/AacEncoderSession.hpp"
#include "clipture/BoundedWrite.hpp"
#include "clipture/VideoTimeline.hpp"

#include <Windows.h>
#include <winioctl.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <thread>
#include <utility>

namespace clipture {
namespace {

using Bytes = std::vector<uint8_t>;

struct SampleInfo {
    std::size_t size = 0;
    uint64_t fileOffset = 0;
    bool keyframe = false;
    uint32_t duration = 1;
};

struct OwnedSample {
    std::vector<std::byte> payload;
    PacketPayloadPtr sharedPayload;
    PacketPayloadReaderPtr payloadReader;
    SampleInfo info;
    int64_t pts100ns = 0;
    uint32_t encoderEpoch = 0;
    int32_t primingFrames = 0;
    uint32_t encodedFrameCount = 1024;
};

std::span<const std::byte> samplePayload(const OwnedSample& sample) {
    if (sample.sharedPayload) return { sample.sharedPayload->data(), sample.sharedPayload->size() };
    return { sample.payload.data(), sample.payload.size() };
}

struct PcmSampleView {
    std::span<const std::byte> payload;
    int64_t pts100ns = 0;
    uint32_t durationFrames = 1;
};

struct PcmAudioTrack {
    std::string sourceId;
    int sampleRate = 48000;
    int channels = 2;
    std::vector<PcmSampleView> samples;
};

struct AacAudioTrack {
    std::string sourceId;
    int sampleRate = 48000;
    int channels = 2;
    Bytes decoderConfig;
    std::vector<OwnedSample> samples;
    int64_t firstPts100ns = 0;
    int32_t primingFrames = 0;
};

using SaveTimingClock = std::chrono::steady_clock;
constexpr uint64_t kMp4Version0MaxDuration = 0xFFFFFFFFULL;

int64_t saveTimingElapsedMs(SaveTimingClock::time_point startedAt) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(SaveTimingClock::now() - startedAt).count();
}

void logMuxSaveTiming(const char* stage, SaveTimingClock::time_point startedAt, const std::string& details = {}) {
    std::cerr << "[save-timing] source=mux stage=" << stage
              << " ms=" << saveTimingElapsedMs(startedAt);
    if (!details.empty()) std::cerr << " " << details;
    std::cerr << std::endl;
}

std::wstring widen(const std::string& value) {
    if (value.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (needed <= 1) return {};
    std::wstring result(static_cast<std::size_t>(needed - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), needed);
    return result;
}

std::string narrow(const std::wstring& value) {
    if (value.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string result(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), needed, nullptr, nullptr);
    return result;
}

std::wstring defaultSaveFolder() {
    wchar_t path[MAX_PATH] {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_MYVIDEO, nullptr, SHGFP_TYPE_CURRENT, path))) {
        return std::wstring(path) + L"\\Clipture";
    }
    return L".\\Clipture";
}

std::wstring clipFilePath(const std::string& saveFolder) {
    std::wstring folder = widen(saveFolder);
    if (folder.empty()) folder = defaultSaveFolder();
    std::filesystem::create_directories(folder);

    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm localTime {};
    localtime_s(&localTime, &time);
    const int hour12 = localTime.tm_hour % 12 == 0 ? 12 : localTime.tm_hour % 12;
    const wchar_t* period = localTime.tm_hour < 12 ? L"AM" : L"PM";

    std::wostringstream path;
    path << folder << L"\\Clipture "
         << std::put_time(&localTime, L"%Y-%m-%d ")
         << std::setw(2) << std::setfill(L'0') << hour12 << L"-"
         << std::setw(2) << std::setfill(L'0') << localTime.tm_min << L"-"
         << std::setw(2) << std::setfill(L'0') << localTime.tm_sec << L" "
         << period << L".mp4";
    return path.str();
}

bool isVideoPacket(const EncodedPacket& packet) {
    return packet.kind == PacketKind::Video && !payloadEmpty(packet);
}

bool isPcmAudioPacket(const EncodedPacket& packet) {
    return packet.kind == PacketKind::Audio &&
        packet.encoderId == "PCM_S16" &&
        packet.sampleRate > 0 &&
        packet.channelCount > 0 &&
        packet.bitsPerSample == 16 &&
        !payloadEmpty(packet);
}

bool isAacAudioPacket(const EncodedPacket& packet) {
    return packet.kind == PacketKind::Audio &&
        packet.codec == PacketCodec::AacLc &&
        packet.sampleRate > 0 &&
        packet.channelCount > 0 &&
        packet.audioFrameCount > 0 &&
        !payloadEmpty(packet);
}

const std::string& packetTrackId(const EncodedPacket& packet) {
    return packet.logicalTrackId.empty() ? packet.sourceId : packet.logicalTrackId;
}

int aacSampleRateIndex(int sampleRate) {
    switch (sampleRate) {
        case 96000: return 0;
        case 88200: return 1;
        case 64000: return 2;
        case 48000: return 3;
        case 44100: return 4;
        case 32000: return 5;
        case 24000: return 6;
        case 22050: return 7;
        case 16000: return 8;
        case 12000: return 9;
        case 11025: return 10;
        case 8000: return 11;
        case 7350: return 12;
        default: return 3;
    }
}

Bytes makeAacAudioSpecificConfig(int sampleRate, int channels) {
    const int objectType = 2; // AAC LC
    const int frequencyIndex = aacSampleRateIndex(sampleRate);
    const int channelConfig = std::clamp(channels, 1, 2);
    const uint16_t bits = static_cast<uint16_t>((objectType << 11) | (frequencyIndex << 7) | (channelConfig << 3));
    return {
        static_cast<uint8_t>((bits >> 8) & 0xFF),
        static_cast<uint8_t>(bits & 0xFF)
    };
}

std::string hresultMessage(const std::string& prefix, HRESULT hr) {
    std::ostringstream out;
    out << prefix << " HRESULT 0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr) << ".";
    return out.str();
}

void appendU8(Bytes& out, uint8_t value) {
    out.push_back(value);
}

void appendU16(Bytes& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void appendU24(Bytes& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void appendU32(Bytes& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void appendU64(Bytes& out, uint64_t value) {
    appendU32(out, static_cast<uint32_t>((value >> 32) & 0xFFFFFFFFULL));
    appendU32(out, static_cast<uint32_t>(value & 0xFFFFFFFFULL));
}

void appendType(Bytes& out, const char type[4]) {
    out.insert(out.end(), type, type + 4);
}

void appendBytes(Bytes& out, const Bytes& bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void appendBytes(Bytes& out, const std::vector<std::byte>& bytes) {
    for (const auto byte : bytes) out.push_back(std::to_integer<uint8_t>(byte));
}

Bytes box(const char type[4], const Bytes& payload) {
    Bytes out;
    appendU32(out, static_cast<uint32_t>(payload.size() + 8));
    appendType(out, type);
    appendBytes(out, payload);
    return out;
}

Bytes fullBox(const char type[4], uint8_t version, uint32_t flags, const Bytes& payload) {
    Bytes full;
    appendU8(full, version);
    appendU24(full, flags);
    appendBytes(full, payload);
    return box(type, full);
}

bool appendSamplePayload(IMFSample* sample, OwnedSample& outSample) {
    if (!sample) return false;

    Microsoft::WRL::ComPtr<IMFMediaBuffer> contiguous;
    if (FAILED(sample->ConvertToContiguousBuffer(&contiguous))) return false;

    BYTE* data = nullptr;
    DWORD maxLength = 0;
    DWORD currentLength = 0;
    if (FAILED(contiguous->Lock(&data, &maxLength, &currentLength))) return false;
    outSample.payload.resize(currentLength);
    if (currentLength > 0 && data) {
        std::memcpy(outSample.payload.data(), data, currentLength);
    }
    contiguous->Unlock();

    outSample.info.size = outSample.payload.size();
    return !outSample.payload.empty();
}

bool drainAacOutput(IMFTransform* encoder, AacAudioTrack& output, bool finalDrain, std::string& error) {
    MFT_OUTPUT_STREAM_INFO streamInfo {};
    HRESULT hr = encoder->GetOutputStreamInfo(0, &streamInfo);
    if (FAILED(hr)) {
        error = hresultMessage("AAC encoder output stream info failed.", hr);
        return false;
    }

    const DWORD bufferSize = std::max<DWORD>(streamInfo.cbSize, 64 * 1024);
    while (true) {
        Microsoft::WRL::ComPtr<IMFSample> outSample;
        hr = MFCreateSample(&outSample);
        if (FAILED(hr)) {
            error = hresultMessage("AAC output sample allocation failed.", hr);
            return false;
        }

        Microsoft::WRL::ComPtr<IMFMediaBuffer> outBuffer;
        hr = MFCreateMemoryBuffer(bufferSize, &outBuffer);
        if (FAILED(hr)) {
            error = hresultMessage("AAC output buffer allocation failed.", hr);
            return false;
        }
        outSample->AddBuffer(outBuffer.Get());

        MFT_OUTPUT_DATA_BUFFER outputBuffer {};
        outputBuffer.dwStreamID = 0;
        outputBuffer.pSample = outSample.Get();
        DWORD processStatus = 0;
        hr = encoder->ProcessOutput(0, 1, &outputBuffer, &processStatus);
        if (outputBuffer.pEvents) outputBuffer.pEvents->Release();

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
        if (FAILED(hr)) {
            if (!finalDrain && hr == MF_E_TRANSFORM_STREAM_CHANGE) return true;
            error = hresultMessage("AAC encoder output failed.", hr);
            return false;
        }

        OwnedSample encoded;
        encoded.info.duration = 1024;
        if (appendSamplePayload(outSample.Get(), encoded)) {
            output.samples.push_back(std::move(encoded));
        }
    }
}

bool encodePcmTrackToAac(const PcmAudioTrack& pcmTrack, AacAudioTrack& aacTrack, std::string& error) {
    if (pcmTrack.samples.empty()) return false;

    MFT_REGISTER_TYPE_INFO outputInfo {};
    outputInfo.guidMajorType = MFMediaType_Audio;
    outputInfo.guidSubtype = MFAudioFormat_AAC;

    IMFActivate** activates = nullptr;
    UINT32 activateCount = 0;
    HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_AUDIO_ENCODER,
        MFT_ENUM_FLAG_ALL,
        nullptr,
        &outputInfo,
        &activates,
        &activateCount);
    if (FAILED(hr) || activateCount == 0 || !activates) {
        error = hresultMessage("No Media Foundation AAC encoder found.", FAILED(hr) ? hr : MF_E_TOPO_CODEC_NOT_FOUND);
        return false;
    }

    Microsoft::WRL::ComPtr<IMFTransform> encoder;
    hr = activates[0]->ActivateObject(IID_PPV_ARGS(&encoder));
    for (UINT32 i = 0; i < activateCount; ++i) activates[i]->Release();
    CoTaskMemFree(activates);
    if (FAILED(hr)) {
        error = hresultMessage("AAC encoder activation failed.", hr);
        return false;
    }

    const int channels = std::clamp(pcmTrack.channels, 1, 2);
    const int sampleRate = std::max(8000, pcmTrack.sampleRate);
    const int audioBitrate = channels == 1 ? 96000 : 160000;

    Microsoft::WRL::ComPtr<IMFMediaType> inputType;
    MFCreateMediaType(&inputType);
    inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    inputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    inputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, static_cast<UINT32>(channels));
    inputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(sampleRate));
    inputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    inputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, static_cast<UINT32>(channels * 2));
    inputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, static_cast<UINT32>(sampleRate * channels * 2));

    hr = encoder->SetInputType(0, inputType.Get(), 0);
    if (FAILED(hr)) {
        error = hresultMessage("AAC encoder input type failed.", hr);
        return false;
    }

    Microsoft::WRL::ComPtr<IMFMediaType> selectedOutputType;
    for (DWORD index = 0;; ++index) {
        Microsoft::WRL::ComPtr<IMFMediaType> candidate;
        hr = encoder->GetOutputAvailableType(0, index, &candidate);
        if (hr == MF_E_NO_MORE_TYPES) break;
        if (FAILED(hr)) continue;

        GUID subtype {};
        if (FAILED(candidate->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFAudioFormat_AAC) continue;

        candidate->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, static_cast<UINT32>(channels));
        candidate->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(sampleRate));
        candidate->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, static_cast<UINT32>(audioBitrate / 8));
        candidate->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        candidate->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        candidate->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);

        hr = encoder->SetOutputType(0, candidate.Get(), 0);
        if (SUCCEEDED(hr)) {
            selectedOutputType = candidate;
            break;
        }
    }

    if (!selectedOutputType) {
        Microsoft::WRL::ComPtr<IMFMediaType> outputType;
        MFCreateMediaType(&outputType);
        outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        outputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        outputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, static_cast<UINT32>(channels));
        outputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(sampleRate));
        outputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, static_cast<UINT32>(audioBitrate / 8));
        outputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        outputType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        outputType->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);

        hr = encoder->SetOutputType(0, outputType.Get(), 0);
        if (FAILED(hr)) {
            error = hresultMessage("AAC encoder output type failed.", hr);
            return false;
        }
        selectedOutputType = outputType;
    }

    encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    int64_t sampleTime = 0;
    for (const auto& pcmSample : pcmTrack.samples) {
        Microsoft::WRL::ComPtr<IMFSample> sample;
        hr = MFCreateSample(&sample);
        if (FAILED(hr)) {
            error = hresultMessage("AAC input sample allocation failed.", hr);
            return false;
        }

        Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
        hr = MFCreateMemoryBuffer(static_cast<DWORD>(pcmSample.payload.size()), &buffer);
        if (FAILED(hr)) {
            error = hresultMessage("AAC input buffer allocation failed.", hr);
            return false;
        }

        BYTE* dest = nullptr;
        DWORD maxLength = 0;
        DWORD currentLength = 0;
        buffer->Lock(&dest, &maxLength, &currentLength);
        std::memcpy(dest, pcmSample.payload.data(), pcmSample.payload.size());
        buffer->Unlock();
        buffer->SetCurrentLength(static_cast<DWORD>(pcmSample.payload.size()));
        sample->AddBuffer(buffer.Get());

        const int64_t duration100ns = static_cast<int64_t>((10'000'000.0 * pcmSample.durationFrames) / sampleRate);
        sample->SetSampleTime(sampleTime);
        sample->SetSampleDuration(duration100ns);
        sampleTime += duration100ns;

        while (true) {
            hr = encoder->ProcessInput(0, sample.Get(), 0);
            if (hr == MF_E_NOTACCEPTING) {
                if (!drainAacOutput(encoder.Get(), aacTrack, false, error)) return false;
                continue;
            }
            if (FAILED(hr)) {
                error = hresultMessage("AAC encoder input failed.", hr);
                return false;
            }
            break;
        }
        if (!drainAacOutput(encoder.Get(), aacTrack, false, error)) return false;
    }

    encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    encoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    if (!drainAacOutput(encoder.Get(), aacTrack, true, error)) return false;

    aacTrack.sourceId = pcmTrack.sourceId;
    aacTrack.sampleRate = sampleRate;
    aacTrack.channels = channels;
    aacTrack.decoderConfig = makeAacAudioSpecificConfig(sampleRate, channels);
    if (!aacTrack.samples.empty()) aacTrack.firstPts100ns = aacTrack.samples.front().pts100ns;
    return !aacTrack.samples.empty();
}

PcmAudioTrack selectUncoveredPcm(const PcmAudioTrack& pcmTrack, const AacAudioTrack* encodedTrack) {
    if (!encodedTrack || encodedTrack->samples.empty()) return pcmTrack;

    struct Coverage {
        int64_t start = 0;
        int64_t end = 0;
    };
    std::vector<Coverage> coverage;
    coverage.reserve(encodedTrack->samples.size());
    for (const auto& sample : encodedTrack->samples) {
        const int64_t duration =
            (static_cast<int64_t>(std::max<uint32_t>(1, sample.encodedFrameCount)) * 10'000'000LL) /
            std::max(1, encodedTrack->sampleRate);
        Coverage next { sample.pts100ns, sample.pts100ns + duration };
        if (!coverage.empty() && next.start <= coverage.back().end + 50'000LL) {
            coverage.back().end = std::max(coverage.back().end, next.end);
        } else {
            coverage.push_back(next);
        }
    }

    PcmAudioTrack missing { pcmTrack.sourceId, pcmTrack.sampleRate, pcmTrack.channels, {} };
    std::size_t coverageIndex = 0;
    for (const auto& sample : pcmTrack.samples) {
        const int64_t sampleEnd = sample.pts100ns +
            (static_cast<int64_t>(sample.durationFrames) * 10'000'000LL) / std::max(1, pcmTrack.sampleRate);
        while (coverageIndex < coverage.size() && coverage[coverageIndex].end < sample.pts100ns) {
            ++coverageIndex;
        }
        const bool covered = coverageIndex < coverage.size() &&
            coverage[coverageIndex].start <= sample.pts100ns + 10'000LL &&
            coverage[coverageIndex].end + 10'000LL >= sampleEnd;
        if (!covered) missing.samples.push_back(sample);
    }
    return missing;
}

void finalizeAudioTimeline(AacAudioTrack& track) {
    std::sort(track.samples.begin(), track.samples.end(), [](const OwnedSample& left, const OwnedSample& right) {
        if (left.pts100ns != right.pts100ns) return left.pts100ns < right.pts100ns;
        return left.encoderEpoch < right.encoderEpoch;
    });
    track.samples.erase(
        std::unique(track.samples.begin(), track.samples.end(), [](const OwnedSample& left, const OwnedSample& right) {
            return std::abs(left.pts100ns - right.pts100ns) <= 10'000LL;
        }),
        track.samples.end());
    if (track.samples.empty()) return;

    track.firstPts100ns = track.samples.front().pts100ns;
    track.primingFrames = std::max<int32_t>(0, track.samples.front().primingFrames);
    for (auto& sample : track.samples) {
        sample.info.duration = std::max<uint32_t>(1, sample.encodedFrameCount);
    }
}

bool encodePcmTrackToAacBatched(const PcmAudioTrack& pcmTrack, AacAudioTrack& aacTrack, std::string& error) {
    if (pcmTrack.samples.empty()) return false;

    AacEncoderSession encoder;
    const int sampleRate = std::max(8000, pcmTrack.sampleRate);
    const int channels = std::clamp(pcmTrack.channels, 1, 2);
    const uint32_t targetBatchFrames = static_cast<uint32_t>(std::max(1, sampleRate / 10));
    const std::size_t bytesPerFrame = static_cast<std::size_t>(channels) * sizeof(int16_t);
    std::vector<std::byte> batch;
    batch.reserve(static_cast<std::size_t>(targetBatchFrames) * bytesPerFrame);
    uint32_t batchFrames = 0;
    int64_t batchPts100ns = 0;
    int64_t expectedInputPts100ns = 0;
    uint32_t epoch = 1;
    bool epochActive = false;
    std::vector<AacEncodedFrame> encodedFrames;

    auto appendEncoded = [&]() {
        for (auto& frame : encodedFrames) {
            OwnedSample sample;
            sample.payload = std::move(frame.payload);
            sample.info.size = sample.payload.size();
            sample.info.duration = frame.durationFrames;
            sample.pts100ns = frame.pts100ns;
            sample.encoderEpoch = epoch;
            sample.primingFrames = frame.primingFrames;
            sample.encodedFrameCount = frame.durationFrames;
            aacTrack.samples.push_back(std::move(sample));
        }
        encodedFrames.clear();
    };
    auto startEpoch = [&]() {
        if (epochActive) return true;
        if (!encoder.start(sampleRate, channels, error)) return false;
        epochActive = true;
        return true;
    };
    auto flushBatch = [&]() {
        if (batchFrames == 0) return true;
        const bool ok = encoder.encode(batch, batchPts100ns, batchFrames, encodedFrames, error);
        if (ok) appendEncoded();
        batch.clear();
        batchFrames = 0;
        return ok;
    };
    auto finishEpoch = [&]() {
        if (!epochActive) return true;
        if (!flushBatch()) return false;
        if (!encoder.finish(encodedFrames, error)) return false;
        appendEncoded();
        encoder.reset();
        epochActive = false;
        expectedInputPts100ns = 0;
        ++epoch;
        return true;
    };

    for (const auto& pcmSample : pcmTrack.samples) {
        const uint32_t availableFrames = static_cast<uint32_t>(pcmSample.payload.size() / bytesPerFrame);
        if (availableFrames == 0) continue;
        constexpr int64_t kContiguousTolerance100ns = 50'000LL;
        if (epochActive && std::abs(pcmSample.pts100ns - expectedInputPts100ns) > kContiguousTolerance100ns) {
            if (!finishEpoch()) return false;
        }
        if (!startEpoch()) return false;
        uint32_t consumedFrames = 0;
        while (consumedFrames < availableFrames) {
            if (batchFrames == 0) {
                batchPts100ns = pcmSample.pts100ns +
                    (static_cast<int64_t>(consumedFrames) * 10'000'000LL) / sampleRate;
            }
            const uint32_t copiedFrames = std::min(targetBatchFrames - batchFrames, availableFrames - consumedFrames);
            const auto byteOffset = static_cast<std::size_t>(consumedFrames) * bytesPerFrame;
            const auto byteCount = static_cast<std::size_t>(copiedFrames) * bytesPerFrame;
            batch.insert(
                batch.end(),
                pcmSample.payload.begin() + static_cast<std::ptrdiff_t>(byteOffset),
                pcmSample.payload.begin() + static_cast<std::ptrdiff_t>(byteOffset + byteCount));
            batchFrames += copiedFrames;
            consumedFrames += copiedFrames;
            if (batchFrames == targetBatchFrames && !flushBatch()) return false;
        }
        expectedInputPts100ns = pcmSample.pts100ns +
            (static_cast<int64_t>(availableFrames) * 10'000'000LL) / sampleRate;
    }
    if (!finishEpoch()) return false;

    aacTrack.sourceId = pcmTrack.sourceId;
    aacTrack.sampleRate = sampleRate;
    aacTrack.channels = channels;
    aacTrack.decoderConfig = makeAacAudioSpecificConfig(sampleRate, channels);
    if (!aacTrack.samples.empty()) aacTrack.firstPts100ns = aacTrack.samples.front().pts100ns;
    return !aacTrack.samples.empty();
}

struct AudioContinuityStats {
    std::size_t insertedSilenceSamples = 0;
    std::size_t removedOverlappingSamples = 0;
    int64_t maximumGap100ns = 0;
};

struct AudioClipAlignmentStats {
    std::size_t droppedLeadingSamples = 0;
    std::size_t droppedTrailingSamples = 0;
    uint64_t droppedLeadingFrames = 0;
    int64_t maximumStartDelay100ns = 0;
};

struct ReusableSilenceSample {
    PacketPayloadPtr payload;
    uint32_t frameCount = 1024;
};

bool makeReusableAacSilence(
    int sampleRate,
    int channels,
    std::vector<ReusableSilenceSample>& silence,
    std::string& error) {
    AacEncoderSession encoder;
    if (!encoder.start(sampleRate, channels, error)) return false;

    const uint32_t inputFrames = static_cast<uint32_t>(std::max(sampleRate, 8'000));
    const std::size_t sampleCount = static_cast<std::size_t>(inputFrames) * std::max(1, channels);
    std::vector<std::byte> zeroPcm(sampleCount * sizeof(int16_t), std::byte { 0 });
    std::vector<AacEncodedFrame> encoded;
    if (!encoder.encode(zeroPcm, 1, inputFrames, encoded, error)) return false;
    if (!encoder.finish(encoded, error)) return false;
    if (encoded.empty()) {
        error = "AAC encoder produced no reusable silence samples.";
        return false;
    }

    const std::size_t first = encoded.size() > 10 ? 4 : 0;
    const std::size_t last = std::min(encoded.size(), first + 16);
    silence.reserve(last - first);
    for (std::size_t index = first; index < last; ++index) {
        auto& frame = encoded[index];
        if (frame.payload.empty()) continue;
        silence.push_back({
            std::make_shared<PacketPayload>(std::move(frame.payload)),
            std::max<uint32_t>(1, frame.durationFrames)
        });
    }
    if (silence.empty()) {
        error = "AAC encoder produced no usable silence samples.";
        return false;
    }
    return true;
}

bool makeAudioTimelineContinuous(
    AacAudioTrack& track,
    AudioContinuityStats& stats,
    std::string& error) {
    if (track.samples.empty()) return true;

    const int sampleRate = std::max(1, track.sampleRate);
    constexpr int64_t kTolerance100ns = 50'000LL;
    auto sampleDuration100ns = [sampleRate](const OwnedSample& sample) {
        return (static_cast<int64_t>(std::max<uint32_t>(1, sample.encodedFrameCount)) * 10'000'000LL) /
            sampleRate;
    };

    std::vector<OwnedSample> continuous;
    continuous.reserve(track.samples.size());
    std::vector<ReusableSilenceSample> reusableSilence;
    std::size_t nextSilence = 0;
    const uint32_t timelineEpoch = std::max<uint32_t>(1, track.samples.front().encoderEpoch);

    auto appendSample = [&](OwnedSample sample, int64_t pts100ns, bool preservePriming) {
        sample.pts100ns = pts100ns;
        sample.encoderEpoch = timelineEpoch;
        if (!preservePriming) sample.primingFrames = 0;
        sample.info.duration = std::max<uint32_t>(1, sample.encodedFrameCount);
        continuous.push_back(std::move(sample));
    };

    const int64_t firstPts100ns = track.samples.front().pts100ns;
    appendSample(std::move(track.samples.front()), firstPts100ns, true);
    int64_t expectedPts100ns = continuous.front().pts100ns + sampleDuration100ns(continuous.front());
    for (std::size_t index = 1; index < track.samples.size(); ++index) {
        auto sample = std::move(track.samples[index]);
        const int64_t duration100ns = sampleDuration100ns(sample);
        const int64_t sampleEnd100ns = sample.pts100ns + duration100ns;
        if (sample.pts100ns < expectedPts100ns - kTolerance100ns ||
            sampleEnd100ns <= expectedPts100ns + kTolerance100ns) {
            ++stats.removedOverlappingSamples;
            continue;
        }

        const int64_t gap100ns = sample.pts100ns - expectedPts100ns;
        stats.maximumGap100ns = std::max(stats.maximumGap100ns, gap100ns);
        if (gap100ns > kTolerance100ns) {
            if (reusableSilence.empty() &&
                !makeReusableAacSilence(sampleRate, track.channels, reusableSilence, error)) {
                return false;
            }
            const uint32_t silenceFrameCount = reusableSilence.front().frameCount;
            const int64_t silenceDuration100ns =
                (static_cast<int64_t>(silenceFrameCount) * 10'000'000LL) / sampleRate;
            const int64_t silenceSamples = std::max<int64_t>(
                0,
                (gap100ns + silenceDuration100ns / 2) / std::max<int64_t>(1, silenceDuration100ns));
            for (int64_t silenceIndex = 0; silenceIndex < silenceSamples; ++silenceIndex) {
                const auto& reusable = reusableSilence[nextSilence++ % reusableSilence.size()];
                OwnedSample silent;
                silent.sharedPayload = reusable.payload;
                silent.info.size = reusable.payload ? reusable.payload->size() : 0;
                silent.info.duration = reusable.frameCount;
                silent.encodedFrameCount = reusable.frameCount;
                appendSample(std::move(silent), expectedPts100ns, false);
                expectedPts100ns += sampleDuration100ns(continuous.back());
                ++stats.insertedSilenceSamples;
            }
        }

        appendSample(std::move(sample), expectedPts100ns, false);
        expectedPts100ns += sampleDuration100ns(continuous.back());
    }

    track.samples = std::move(continuous);
    track.firstPts100ns = track.samples.front().pts100ns;
    track.primingFrames = std::max<int32_t>(0, track.samples.front().primingFrames);
    int64_t verifiedNextPts100ns = track.samples.front().pts100ns;
    for (const auto& sample : track.samples) {
        if (sample.encoderEpoch != timelineEpoch ||
            std::abs(sample.pts100ns - verifiedNextPts100ns) > kTolerance100ns) {
            error = "AAC timeline remained discontinuous after normalization.";
            return false;
        }
        verifiedNextPts100ns = sample.pts100ns + sampleDuration100ns(sample);
    }
    return true;
}

void alignAudioTrackToVideo(
    AacAudioTrack& track,
    int64_t videoStartPts100ns,
    uint64_t movieDuration100ns,
    AudioClipAlignmentStats& stats) {
    if (track.samples.empty()) return;

    const int sampleRate = std::max(1, track.sampleRate);
    const int64_t originalStartPts100ns = track.samples.front().pts100ns;
    const int64_t leadingDuration100ns = std::max<int64_t>(
        0,
        videoStartPts100ns - originalStartPts100ns);
    const uint64_t leadingFrames =
        (static_cast<uint64_t>(leadingDuration100ns) * static_cast<uint64_t>(sampleRate) + 9'999'999ULL) /
        10'000'000ULL;
    const uint64_t framesToDiscard = leadingFrames + static_cast<uint64_t>(
        std::max<int32_t>(0, track.samples.front().primingFrames));

    uint64_t discardedFrames = 0;
    std::size_t discardedSamples = 0;
    while (discardedSamples < track.samples.size() && discardedFrames < framesToDiscard) {
        discardedFrames += std::max<uint32_t>(
            1,
            track.samples[discardedSamples].encodedFrameCount);
        ++discardedSamples;
    }
    if (discardedSamples > 0) {
        track.samples.erase(
            track.samples.begin(),
            track.samples.begin() + static_cast<std::ptrdiff_t>(discardedSamples));
        stats.droppedLeadingSamples += discardedSamples;
        stats.droppedLeadingFrames += discardedFrames;
    }
    if (track.samples.empty()) return;

    // MP4 edit lists that seek into the first AAC sample expose the discarded
    // packets with negative timestamps. Chromium rejects those files outright.
    // Starting on the next complete AAC frame costs at most one frame of silence
    // (about 21 ms at 48 kHz) and keeps every demuxed timestamp non-negative.
    track.samples.front().primingFrames = 0;
    track.primingFrames = 0;
    track.samples.front().pts100ns = std::max(
        videoStartPts100ns,
        track.samples.front().pts100ns);

    const int64_t maximumMovieDuration = static_cast<int64_t>(
        std::min<uint64_t>(movieDuration100ns, static_cast<uint64_t>(std::numeric_limits<int64_t>::max())));
    const int64_t videoEndPts100ns = videoStartPts100ns > std::numeric_limits<int64_t>::max() - maximumMovieDuration
        ? std::numeric_limits<int64_t>::max()
        : videoStartPts100ns + maximumMovieDuration;
    const auto trailing = std::lower_bound(
        track.samples.begin(),
        track.samples.end(),
        videoEndPts100ns,
        [](const OwnedSample& sample, int64_t endPts100ns) {
            return sample.pts100ns < endPts100ns;
        });
    stats.droppedTrailingSamples += static_cast<std::size_t>(
        std::distance(trailing, track.samples.end()));
    track.samples.erase(trailing, track.samples.end());
    if (track.samples.empty()) return;

    track.firstPts100ns = track.samples.front().pts100ns;
    stats.maximumStartDelay100ns = std::max(
        stats.maximumStartDelay100ns,
        std::max<int64_t>(0, track.firstPts100ns - videoStartPts100ns));
}

bool startCodeAt(std::span<const std::byte> data, std::size_t offset, std::size_t& size) {
    if (offset + 3 <= data.size() &&
        data[offset] == std::byte{0} &&
        data[offset + 1] == std::byte{0} &&
        data[offset + 2] == std::byte{1}) {
        size = 3;
        return true;
    }
    if (offset + 4 <= data.size() &&
        data[offset] == std::byte{0} &&
        data[offset + 1] == std::byte{0} &&
        data[offset + 2] == std::byte{0} &&
        data[offset + 3] == std::byte{1}) {
        size = 4;
        return true;
    }
    return false;
}

std::size_t findStartCode(std::span<const std::byte> data, std::size_t offset, std::size_t& size) {
    for (std::size_t i = offset; i + 3 <= data.size(); ++i) {
        if (startCodeAt(data, i, size)) return i;
    }
    return std::string::npos;
}

using NalUnit = H264NalSpan;

struct VideoSamplePlan {
    const EncodedPacket* packet = nullptr;
    std::vector<NalUnit> writableNalus;
    SampleInfo info;
};

std::vector<NalUnit> parseAnnexBNalus(std::span<const std::byte> data) {
    std::vector<NalUnit> nalus;
    std::size_t offset = 0;

    while (offset < data.size()) {
        std::size_t startCodeSize = 0;
        const std::size_t start = findStartCode(data, offset, startCodeSize);
        if (start == std::string::npos) break;

        const std::size_t nalStart = start + startCodeSize;
        if (nalStart >= data.size()) break;

        std::size_t nextStartCodeSize = 0;
        const std::size_t next = findStartCode(data, nalStart, nextStartCodeSize);
        const std::size_t nalEnd = next == std::string::npos ? data.size() : next;
        if (nalEnd > nalStart) {
            const auto header = std::to_integer<uint8_t>(data[nalStart]);
            nalus.push_back({
                static_cast<uint32_t>(nalStart),
                static_cast<uint32_t>(nalEnd - nalStart),
                static_cast<uint8_t>(header & 0x1F)
            });
        }
        offset = nalEnd;
    }

    return nalus;
}

std::vector<std::byte> copyNaluPayload(const EncodedPacket& packet, const NalUnit& nalu) {
    std::vector<std::byte> result;
    if (!copyPayloadRange(packet, nalu.offset, nalu.size, result)) result.clear();
    return result;
}

Bytes buildAvcDecoderConfig(const std::vector<std::byte>& sps, const std::vector<std::byte>& pps) {
    if (sps.size() < 4 || pps.empty() || sps.size() > 0xFFFF || pps.size() > 0xFFFF) return {};

    Bytes config;
    config.reserve(11 + sps.size() + pps.size());
    appendU8(config, 1);
    appendU8(config, std::to_integer<uint8_t>(sps[1]));
    appendU8(config, std::to_integer<uint8_t>(sps[2]));
    appendU8(config, std::to_integer<uint8_t>(sps[3]));
    appendU8(config, 0xFF);
    appendU8(config, 0xE1);
    appendU16(config, static_cast<uint16_t>(sps.size()));
    appendBytes(config, sps);
    appendU8(config, 1);
    appendU16(config, static_cast<uint16_t>(pps.size()));
    appendBytes(config, pps);
    return config;
}

bool isWritableVideoNalu(const NalUnit& nalu) {
    return nalu.type != 7 &&
        nalu.type != 8 &&
        nalu.type != 9 &&
        nalu.size > 0 &&
        nalu.size <= 0xFFFFFFFFULL;
}

std::size_t avccSampleSize(std::span<const NalUnit> nalus) {
    std::size_t size = 0;
    for (const auto& nalu : nalus) {
        size += sizeof(uint32_t) + nalu.size;
    }
    return size;
}

Bytes makeFtyp() {
    Bytes payload;
    appendType(payload, "isom");
    appendU32(payload, 0x00000200);
    appendType(payload, "isom");
    appendType(payload, "iso2");
    appendType(payload, "avc1");
    appendType(payload, "mp41");
    appendType(payload, "qt  ");
    return box("ftyp", payload);
}

Bytes makeMvhd(uint32_t timescale, uint64_t duration, uint32_t nextTrackId) {
    Bytes payload;
    const bool version1 = duration > kMp4Version0MaxDuration;
    if (version1) {
        appendU64(payload, 0);
        appendU64(payload, 0);
        appendU32(payload, timescale);
        appendU64(payload, duration);
    } else {
        appendU32(payload, 0);
        appendU32(payload, 0);
        appendU32(payload, timescale);
        appendU32(payload, static_cast<uint32_t>(duration));
    }
    appendU32(payload, 0x00010000);
    appendU16(payload, 0x0100);
    appendU16(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0x00010000);
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0x00010000);
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0x40000000);
    for (int i = 0; i < 6; ++i) appendU32(payload, 0);
    appendU32(payload, nextTrackId);
    return fullBox("mvhd", version1 ? 1 : 0, 0, payload);
}

Bytes makeTkhd(uint64_t duration, int width, int height) {
    Bytes payload;
    const bool version1 = duration > kMp4Version0MaxDuration;
    if (version1) {
        appendU64(payload, 0);
        appendU64(payload, 0);
        appendU32(payload, 1);
        appendU32(payload, 0);
        appendU64(payload, duration);
    } else {
        appendU32(payload, 0);
        appendU32(payload, 0);
        appendU32(payload, 1);
        appendU32(payload, 0);
        appendU32(payload, static_cast<uint32_t>(duration));
    }
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU16(payload, 0);
    appendU16(payload, 0);
    appendU16(payload, 0);
    appendU16(payload, 0);
    appendU32(payload, 0x00010000);
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0x00010000);
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0x40000000);
    appendU32(payload, static_cast<uint32_t>(std::max(1, width)) << 16);
    appendU32(payload, static_cast<uint32_t>(std::max(1, height)) << 16);
    return fullBox("tkhd", version1 ? 1 : 0, 0x000007, payload);
}

Bytes makeAudioTkhd(uint32_t trackId, uint64_t duration) {
    Bytes payload;
    const bool version1 = duration > kMp4Version0MaxDuration;
    if (version1) {
        appendU64(payload, 0);
        appendU64(payload, 0);
        appendU32(payload, trackId);
        appendU32(payload, 0);
        appendU64(payload, duration);
    } else {
        appendU32(payload, 0);
        appendU32(payload, 0);
        appendU32(payload, trackId);
        appendU32(payload, 0);
        appendU32(payload, static_cast<uint32_t>(duration));
    }
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU16(payload, 0);
    appendU16(payload, 0);
    appendU16(payload, 0x0100);
    appendU16(payload, 0);
    appendU32(payload, 0x00010000);
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0x00010000);
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0x40000000);
    appendU32(payload, 0);
    appendU32(payload, 0);
    return fullBox("tkhd", version1 ? 1 : 0, 0x000007, payload);
}

Bytes makeAudioEdts(
    const AacAudioTrack& track,
    uint64_t movieDuration100ns,
    int64_t videoStartPts100ns) {
    if (track.samples.empty() || movieDuration100ns == 0) return {};
    const int sampleRate = std::max(1, track.sampleRate);
    struct EditEntry {
        uint64_t duration100ns = 0;
        int64_t mediaTimeFrames = -1;
    };
    std::vector<EditEntry> edits;

    uint64_t mediaDurationFrames = 0;
    for (const auto& sample : track.samples) {
        mediaDurationFrames += std::max<uint32_t>(1, sample.encodedFrameCount);
    }
    const int64_t relativeStart100ns = std::max<int64_t>(
        0,
        track.samples.front().pts100ns - videoStartPts100ns);
    const uint64_t presentationStart100ns = std::min<uint64_t>(
        static_cast<uint64_t>(relativeStart100ns),
        movieDuration100ns);
    if (presentationStart100ns > 0) edits.push_back({ presentationStart100ns, -1 });

    uint64_t presentationCursor100ns = presentationStart100ns;
    if (mediaDurationFrames > 0 && presentationCursor100ns < movieDuration100ns) {
        const uint64_t playableDuration100ns = std::min(
            (mediaDurationFrames * 10'000'000ULL) / static_cast<uint64_t>(sampleRate),
            movieDuration100ns - presentationCursor100ns);
        if (playableDuration100ns > 0) {
            edits.push_back({ playableDuration100ns, 0 });
            presentationCursor100ns += playableDuration100ns;
        }
    }
    if (presentationCursor100ns < movieDuration100ns) {
        edits.push_back({ movieDuration100ns - presentationCursor100ns, -1 });
    }
    if (edits.empty()) return {};

    Bytes entries;
    appendU32(entries, static_cast<uint32_t>(edits.size()));
    for (const auto& edit : edits) {
        appendU64(entries, edit.duration100ns);
        appendU64(entries, static_cast<uint64_t>(edit.mediaTimeFrames));
        appendU16(entries, 1);
        appendU16(entries, 0);
    }
    return box("edts", fullBox("elst", 1, 0, entries));
}

Bytes makeVideoEdts(uint64_t movieDuration100ns, uint64_t mediaStart100ns) {
    if (movieDuration100ns == 0) return {};
    Bytes entries;
    appendU32(entries, 1);
    appendU64(entries, movieDuration100ns);
    appendU64(entries, mediaStart100ns);
    appendU16(entries, 1);
    appendU16(entries, 0);
    return box("edts", fullBox("elst", 1, 0, entries));
}

Bytes makeMdhd(uint32_t timescale, uint64_t duration) {
    Bytes payload;
    const bool version1 = duration > kMp4Version0MaxDuration;
    if (version1) {
        appendU64(payload, 0);
        appendU64(payload, 0);
        appendU32(payload, timescale);
        appendU64(payload, duration);
    } else {
        appendU32(payload, 0);
        appendU32(payload, 0);
        appendU32(payload, timescale);
        appendU32(payload, static_cast<uint32_t>(duration));
    }
    appendU16(payload, 0x55C4);
    appendU16(payload, 0);
    return fullBox("mdhd", version1 ? 1 : 0, 0, payload);
}

Bytes makeHdlr() {
    Bytes payload;
    appendU32(payload, 0);
    appendType(payload, "vide");
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0);
    const char name[] = "VideoHandler";
    payload.insert(payload.end(), name, name + sizeof(name));
    return fullBox("hdlr", 0, 0, payload);
}

Bytes makeAudioHdlr() {
    Bytes payload;
    appendU32(payload, 0);
    appendType(payload, "soun");
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 0);
    const char name[] = "SoundHandler";
    payload.insert(payload.end(), name, name + sizeof(name));
    return fullBox("hdlr", 0, 0, payload);
}

Bytes makeVmhd() {
    Bytes payload;
    appendU16(payload, 0);
    appendU16(payload, 0);
    appendU16(payload, 0);
    appendU16(payload, 0);
    return fullBox("vmhd", 0, 1, payload);
}

Bytes makeSmhd() {
    Bytes payload;
    appendU16(payload, 0);
    appendU16(payload, 0);
    return fullBox("smhd", 0, 0, payload);
}

Bytes makeDinf() {
    Bytes url = fullBox("url ", 0, 1, {});
    Bytes drefPayload;
    appendU32(drefPayload, 1);
    appendBytes(drefPayload, url);
    return box("dinf", fullBox("dref", 0, 0, drefPayload));
}

Bytes makeStsd(const Bytes& avcConfig, int width, int height) {
    Bytes avc1;
    for (int i = 0; i < 6; ++i) appendU8(avc1, 0);
    appendU16(avc1, 1);
    appendU16(avc1, 0);
    appendU16(avc1, 0);
    appendU32(avc1, 0);
    appendU32(avc1, 0);
    appendU32(avc1, 0);
    appendU16(avc1, static_cast<uint16_t>(std::max(1, width)));
    appendU16(avc1, static_cast<uint16_t>(std::max(1, height)));
    appendU32(avc1, 0x00480000);
    appendU32(avc1, 0x00480000);
    appendU32(avc1, 0);
    appendU16(avc1, 1);
    appendU8(avc1, 0);
    for (int i = 0; i < 31; ++i) appendU8(avc1, 0);
    appendU16(avc1, 0x0018);
    appendU16(avc1, 0xFFFF);
    appendBytes(avc1, box("avcC", avcConfig));

    Bytes payload;
    appendU32(payload, 1);
    appendBytes(payload, box("avc1", avc1));
    return fullBox("stsd", 0, 0, payload);
}

void appendDescriptorLength(Bytes& out, std::size_t length) {
    uint8_t bytes[4] {};
    bytes[3] = static_cast<uint8_t>(length & 0x7F);
    bytes[2] = static_cast<uint8_t>((length >> 7) & 0x7F);
    bytes[1] = static_cast<uint8_t>((length >> 14) & 0x7F);
    bytes[0] = static_cast<uint8_t>((length >> 21) & 0x7F);
    bytes[0] |= 0x80;
    bytes[1] |= 0x80;
    bytes[2] |= 0x80;
    out.insert(out.end(), bytes, bytes + 4);
}

Bytes makeEsds(const Bytes& decoderConfig, int audioBitrate) {
    Bytes decoderSpecific;
    appendU8(decoderSpecific, 0x05);
    appendDescriptorLength(decoderSpecific, decoderConfig.size());
    appendBytes(decoderSpecific, decoderConfig);

    Bytes decoderConfigDescriptor;
    appendU8(decoderConfigDescriptor, 0x40); // MPEG-4 Audio
    appendU8(decoderConfigDescriptor, 0x15); // AudioStream
    appendU24(decoderConfigDescriptor, 0);
    appendU32(decoderConfigDescriptor, static_cast<uint32_t>(audioBitrate));
    appendU32(decoderConfigDescriptor, static_cast<uint32_t>(audioBitrate));
    appendBytes(decoderConfigDescriptor, decoderSpecific);

    Bytes decoderDescriptor;
    appendU8(decoderDescriptor, 0x04);
    appendDescriptorLength(decoderDescriptor, decoderConfigDescriptor.size());
    appendBytes(decoderDescriptor, decoderConfigDescriptor);

    Bytes slConfig;
    appendU8(slConfig, 0x06);
    appendDescriptorLength(slConfig, 1);
    appendU8(slConfig, 0x02);

    Bytes esDescriptor;
    appendU16(esDescriptor, 1);
    appendU8(esDescriptor, 0);
    appendBytes(esDescriptor, decoderDescriptor);
    appendBytes(esDescriptor, slConfig);

    Bytes payload;
    appendU8(payload, 0x03);
    appendDescriptorLength(payload, esDescriptor.size());
    appendBytes(payload, esDescriptor);
    return fullBox("esds", 0, 0, payload);
}

Bytes makeAudioStsd(const AacAudioTrack& track) {
    Bytes sampleEntry;
    for (int i = 0; i < 6; ++i) appendU8(sampleEntry, 0);
    appendU16(sampleEntry, 1);
    appendU16(sampleEntry, 0);
    appendU16(sampleEntry, 0);
    appendU32(sampleEntry, 0);
    appendU16(sampleEntry, static_cast<uint16_t>(std::clamp(track.channels, 1, 2)));
    appendU16(sampleEntry, 16);
    appendU16(sampleEntry, 0);
    appendU16(sampleEntry, 0);
    appendU32(sampleEntry, static_cast<uint32_t>(std::max(1, track.sampleRate)) << 16);
    appendBytes(sampleEntry, makeEsds(track.decoderConfig, track.channels == 1 ? 96000 : 160000));

    Bytes payload;
    appendU32(payload, 1);
    appendBytes(payload, box("mp4a", sampleEntry));
    return fullBox("stsd", 0, 0, payload);
}

const SampleInfo& sampleInfo(const SampleInfo& sample) {
    return sample;
}

const SampleInfo& sampleInfo(const OwnedSample& sample) {
    return sample.info;
}

const SampleInfo& sampleInfo(const VideoSamplePlan& sample) {
    return sample.info;
}

template <typename Samples>
Bytes makeStts(const Samples& samples) {
    Bytes payload;
    std::vector<std::pair<uint32_t, uint32_t>> entries;
    for (const auto& sample : samples) {
        const uint32_t duration = std::max<uint32_t>(1, sampleInfo(sample).duration);
        if (!entries.empty() && entries.back().second == duration) {
            ++entries.back().first;
        } else {
            entries.emplace_back(1, duration);
        }
    }
    appendU32(payload, static_cast<uint32_t>(entries.size()));
    for (const auto& [count, duration] : entries) {
        appendU32(payload, count);
        appendU32(payload, duration);
    }
    return fullBox("stts", 0, 0, payload);
}

template <typename Samples>
Bytes makeStss(const Samples& samples) {
    Bytes payload;
    std::vector<uint32_t> syncSamples;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if (sampleInfo(samples[i]).keyframe) syncSamples.push_back(static_cast<uint32_t>(i + 1));
    }
    appendU32(payload, static_cast<uint32_t>(syncSamples.size()));
    for (const auto sampleNumber : syncSamples) appendU32(payload, sampleNumber);
    return fullBox("stss", 0, 0, payload);
}

Bytes makeStsc() {
    Bytes payload;
    appendU32(payload, 1);
    appendU32(payload, 1);
    appendU32(payload, 1);
    appendU32(payload, 1);
    return fullBox("stsc", 0, 0, payload);
}

template <typename Samples>
Bytes makeStsz(const Samples& samples) {
    Bytes payload;
    appendU32(payload, 0);
    appendU32(payload, static_cast<uint32_t>(samples.size()));
    for (const auto& sample : samples) appendU32(payload, static_cast<uint32_t>(sampleInfo(sample).size));
    return fullBox("stsz", 0, 0, payload);
}

template <typename Samples>
Bytes makeCo64(const Samples& samples) {
    Bytes payload;
    appendU32(payload, static_cast<uint32_t>(samples.size()));
    for (const auto& sample : samples) appendU64(payload, sampleInfo(sample).fileOffset);
    return fullBox("co64", 0, 0, payload);
}

template <typename Samples>
uint64_t samplesDuration(const Samples& samples) {
    uint64_t duration = 0;
    for (const auto& sample : samples) duration += std::max<uint32_t>(1, sampleInfo(sample).duration);
    return duration;
}

Bytes makeVideoTrak(
    const Bytes& avcConfig,
    const std::vector<VideoSamplePlan>& samples,
    int width,
    int height,
    int /*fps*/,
    uint64_t movieDuration100ns,
    uint64_t mediaStart100ns) {
    const uint32_t timescale = 10'000'000u; // 100ns units — matches PTS-based sample durations
    const uint64_t mediaDuration = samplesDuration(samples);

    Bytes stblPayload;
    appendBytes(stblPayload, makeStsd(avcConfig, width, height));
    appendBytes(stblPayload, makeStts(samples));
    appendBytes(stblPayload, makeStss(samples));
    appendBytes(stblPayload, makeStsc());
    appendBytes(stblPayload, makeStsz(samples));
    appendBytes(stblPayload, makeCo64(samples));

    Bytes minfPayload;
    appendBytes(minfPayload, makeVmhd());
    appendBytes(minfPayload, makeDinf());
    appendBytes(minfPayload, box("stbl", stblPayload));

    Bytes mdiaPayload;
    appendBytes(mdiaPayload, makeMdhd(timescale, mediaDuration));
    appendBytes(mdiaPayload, makeHdlr());
    appendBytes(mdiaPayload, box("minf", minfPayload));

    Bytes trakPayload;
    appendBytes(trakPayload, makeTkhd(movieDuration100ns, width, height));
    appendBytes(trakPayload, makeVideoEdts(movieDuration100ns, mediaStart100ns));
    appendBytes(trakPayload, box("mdia", mdiaPayload));
    return box("trak", trakPayload);
}

Bytes makeAudioTrak(
    const AacAudioTrack& track,
    uint32_t trackId,
    uint64_t movieDuration,
    int64_t videoStartPts100ns) {
    const uint32_t timescale = static_cast<uint32_t>(std::max(1, track.sampleRate));
    const uint64_t duration = samplesDuration(track.samples);

    Bytes stblPayload;
    appendBytes(stblPayload, makeAudioStsd(track));
    appendBytes(stblPayload, makeStts(track.samples));
    appendBytes(stblPayload, makeStsc());
    appendBytes(stblPayload, makeStsz(track.samples));
    appendBytes(stblPayload, makeCo64(track.samples));

    Bytes minfPayload;
    appendBytes(minfPayload, makeSmhd());
    appendBytes(minfPayload, makeDinf());
    appendBytes(minfPayload, box("stbl", stblPayload));

    Bytes mdiaPayload;
    appendBytes(mdiaPayload, makeMdhd(timescale, duration));
    appendBytes(mdiaPayload, makeAudioHdlr());
    appendBytes(mdiaPayload, box("minf", minfPayload));

    Bytes trakPayload;
    appendBytes(trakPayload, makeAudioTkhd(trackId, movieDuration));
    if (const auto edits = makeAudioEdts(track, movieDuration, videoStartPts100ns); !edits.empty()) {
        appendBytes(trakPayload, edits);
    }
    appendBytes(trakPayload, box("mdia", mdiaPayload));
    return box("trak", trakPayload);
}

Bytes makeMoov(
    const Bytes& avcConfig,
    const std::vector<VideoSamplePlan>& samples,
    const std::vector<AacAudioTrack>& audioTracks,
    int width,
    int height,
    int fps,
    uint64_t movieDuration100ns,
    uint64_t videoMediaStart100ns,
    int64_t videoPresentationStartPts100ns) {
    const uint32_t timescale = 10'000'000u; // movie timescale in 100ns units — matches video track

    Bytes moovPayload;
    appendBytes(moovPayload, makeMvhd(timescale, movieDuration100ns, static_cast<uint32_t>(audioTracks.size() + 2)));
    appendBytes(moovPayload, makeVideoTrak(
        avcConfig,
        samples,
        width,
        height,
        fps,
        movieDuration100ns,
        videoMediaStart100ns));
    uint32_t trackId = 2;
    for (const auto& audioTrack : audioTracks) {
        appendBytes(moovPayload, makeAudioTrak(
            audioTrack,
            trackId++,
            movieDuration100ns,
            videoPresentationStartPts100ns));
    }
    return box("moov", moovPayload);
}

StorageSeekPenalty queryStorageSeekPenalty(HANDLE handle) {
    if (handle == INVALID_HANDLE_VALUE) return StorageSeekPenalty::Unknown;
    STORAGE_PROPERTY_QUERY query {};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR descriptor {};
    DWORD returnedBytes = 0;
    if (!DeviceIoControl(
            handle,
            IOCTL_STORAGE_QUERY_PROPERTY,
            &query,
            sizeof(query),
            &descriptor,
            sizeof(descriptor),
            &returnedBytes,
            nullptr) ||
        returnedBytes < sizeof(descriptor)) {
        return StorageSeekPenalty::Unknown;
    }
    return descriptor.IncursSeekPenalty
        ? StorageSeekPenalty::Incurs
        : StorageSeekPenalty::DoesNotIncur;
}

StorageSeekPenalty storageSeekPenaltyForPath(const std::wstring& path, HANDLE fileHandle) {
    const auto directResult = queryStorageSeekPenalty(fileHandle);
    if (directResult != StorageSeekPenalty::Unknown) return directResult;

    std::wstring fullPath(32'768, L'\0');
    const DWORD fullPathLength = GetFullPathNameW(
        path.c_str(),
        static_cast<DWORD>(fullPath.size()),
        fullPath.data(),
        nullptr);
    if (fullPathLength == 0 || fullPathLength >= fullPath.size()) return StorageSeekPenalty::Unknown;
    fullPath.resize(fullPathLength);

    std::wstring volumePath(32'768, L'\0');
    if (!GetVolumePathNameW(
            fullPath.c_str(),
            volumePath.data(),
            static_cast<DWORD>(volumePath.size()))) {
        return StorageSeekPenalty::Unknown;
    }
    volumePath.resize(volumePath.find(L'\0'));

    const UINT driveType = GetDriveTypeW(volumePath.c_str());
    if (driveType == DRIVE_RAMDISK) return StorageSeekPenalty::DoesNotIncur;
    if (driveType == DRIVE_REMOTE || driveType == DRIVE_CDROM || driveType == DRIVE_NO_ROOT_DIR) {
        return StorageSeekPenalty::Unknown;
    }

    std::wstring volumeName(32'768, L'\0');
    std::wstring volumeDevice;
    if (GetVolumeNameForVolumeMountPointW(
            volumePath.c_str(),
            volumeName.data(),
            static_cast<DWORD>(volumeName.size()))) {
        volumeName.resize(volumeName.find(L'\0'));
        while (!volumeName.empty() && (volumeName.back() == L'\\' || volumeName.back() == L'/')) {
            volumeName.pop_back();
        }
        volumeDevice = std::move(volumeName);
    } else if (volumePath.size() >= 2 && volumePath[1] == L':') {
        volumeDevice = L"\\\\.\\" + volumePath.substr(0, 2);
    } else {
        return StorageSeekPenalty::Unknown;
    }

    const HANDLE volumeHandle = CreateFileW(
        volumeDevice.c_str(),
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
    if (volumeHandle == INVALID_HANDLE_VALUE) return StorageSeekPenalty::Unknown;
    const auto result = queryStorageSeekPenalty(volumeHandle);
    CloseHandle(volumeHandle);
    return result;
}

const char* storageSeekPenaltyName(StorageSeekPenalty value) {
    switch (value) {
        case StorageSeekPenalty::DoesNotIncur: return "none";
        case StorageSeekPenalty::Incurs: return "yes";
        default: return "unknown";
    }
}

const char* ioPriorityName(PRIORITY_HINT value) {
    switch (value) {
        case IoPriorityHintVeryLow: return "very-low";
        case IoPriorityHintLow: return "low";
        case IoPriorityHintNormal: return "normal";
        default: return "unknown";
    }
}

class Win32FileWriter {
public:
    static constexpr DWORD maxWriteBytes = 512u * 1024u;

    explicit Win32FileWriter(const std::wstring& path) {
        handle_ = CreateFileW(
            path.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            lastError_ = GetLastError();
            return;
        }

        storageSeekPenalty_ = storageSeekPenaltyForPath(path, handle_);

        FILE_IO_PRIORITY_HINT_INFO priorityInfo {};
        priorityInfo.PriorityHint = storageSeekPenalty_ == StorageSeekPenalty::DoesNotIncur
            ? IoPriorityHintLow
            : IoPriorityHintVeryLow;
        ioPriorityHint_ = priorityInfo.PriorityHint;
        lowPriorityApplied_ = SetFileInformationByHandle(
            handle_,
            FileIoPriorityHintInfo,
            &priorityInfo,
            sizeof(priorityInfo)) != FALSE;
    }

    ~Win32FileWriter() {
        close();
    }

    bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }
    bool good() const { return valid() && lastError_ == ERROR_SUCCESS; }
    bool preallocated() const { return preallocated_; }
    bool lowPriorityApplied() const { return lowPriorityApplied_; }
    PRIORITY_HINT ioPriorityHint() const { return ioPriorityHint_; }
    StorageSeekPenalty storageSeekPenalty() const { return storageSeekPenalty_; }
    uint64_t bytesWritten() const { return bytesWritten_; }
    DWORD maximumWriteSize() const { return maximumWriteSize_; }
    DWORD lastError() const { return lastError_; }

    bool preallocate(uint64_t size) {
        if (!good() || size > static_cast<uint64_t>(std::numeric_limits<LONGLONG>::max())) return false;
        LARGE_INTEGER target {};
        target.QuadPart = static_cast<LONGLONG>(size);
        LARGE_INTEGER beginning {};
        FILE_ALLOCATION_INFO allocation {};
        allocation.AllocationSize = target;
        const bool allocationReserved = SetFileInformationByHandle(
            handle_,
            FileAllocationInfo,
            &allocation,
            sizeof(allocation)) != FALSE;
        if (!SetFilePointerEx(handle_, target, nullptr, FILE_BEGIN) || !SetEndOfFile(handle_)) {
            SetFilePointerEx(handle_, beginning, nullptr, FILE_BEGIN);
            return false;
        }
        if (!SetFilePointerEx(handle_, beginning, nullptr, FILE_BEGIN)) return false;
        preallocated_ = allocationReserved;
        return preallocated_;
    }

    bool write(std::span<const std::byte> bytes) {
        while (!bytes.empty() && good()) {
            const DWORD request = static_cast<DWORD>(boundedWriteSize(bytes.size(), maxWriteBytes));
            DWORD written = 0;
            if (!WriteFile(handle_, bytes.data(), request, &written, nullptr) || written != request) {
                lastError_ = GetLastError();
                if (lastError_ == ERROR_SUCCESS) lastError_ = ERROR_WRITE_FAULT;
                return false;
            }
            maximumWriteSize_ = std::max(maximumWriteSize_, request);
            bytesWritten_ += written;
            bytes = bytes.subspan(written);
        }
        return good();
    }

    bool close() {
        if (handle_ == INVALID_HANDLE_VALUE) return lastError_ == ERROR_SUCCESS;
        const HANDLE handle = std::exchange(handle_, INVALID_HANDLE_VALUE);
        if (!CloseHandle(handle)) {
            lastError_ = GetLastError();
            return false;
        }
        return lastError_ == ERROR_SUCCESS;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    DWORD lastError_ = ERROR_SUCCESS;
    DWORD maximumWriteSize_ = 0;
    uint64_t bytesWritten_ = 0;
    bool preallocated_ = false;
    bool lowPriorityApplied_ = false;
    PRIORITY_HINT ioPriorityHint_ = IoPriorityHintNormal;
    StorageSeekPenalty storageSeekPenalty_ = StorageSeekPenalty::Unknown;
};

void writeU32(Win32FileWriter& out, uint32_t value) {
    const std::byte bytes[] = {
        static_cast<std::byte>((value >> 24) & 0xFF),
        static_cast<std::byte>((value >> 16) & 0xFF),
        static_cast<std::byte>((value >> 8) & 0xFF),
        static_cast<std::byte>(value & 0xFF)
    };
    out.write(bytes);
}

void writeU64(Win32FileWriter& out, uint64_t value) {
    writeU32(out, static_cast<uint32_t>((value >> 32) & 0xFFFFFFFFULL));
    writeU32(out, static_cast<uint32_t>(value & 0xFFFFFFFFULL));
}

void writeType(Win32FileWriter& out, const char type[4]) {
    out.write(std::as_bytes(std::span<const char>(type, 4)));
}

void writeBytes(Win32FileWriter& out, const Bytes& bytes) {
    out.write(std::as_bytes(std::span<const uint8_t>(bytes.data(), bytes.size())));
}

class BufferedByteWriter {
public:
    explicit BufferedByteWriter(
        Win32FileWriter& out,
        std::size_t capacity = 4 * 1024 * 1024,
        std::function<MuxPressureSample()> samplePressure = {},
        bool adaptiveRateEnabled = false,
        AdaptiveWritePacerConfig adaptiveRate = {},
        std::size_t burstBytes = 0)
        : out_(out),
          capacity_(std::max<std::size_t>(capacity, 1)),
          samplePressure_(std::move(samplePressure)),
          adaptiveRateEnabled_(adaptiveRateEnabled),
          rateController_(adaptiveRate),
          burstBytes_(adaptiveRateEnabled
              ? std::max<std::size_t>(burstBytes, capacity_)
              : 0) {
        buffer_.reserve(capacity_);
    }

    ~BufferedByteWriter() {
        finishBackgroundMode();
    }

    void write(std::span<const std::byte> bytes) {
        while (!bytes.empty()) {
            const std::size_t available = capacity_ - buffer_.size();
            if (available == 0) {
                flush();
                continue;
            }

            const std::size_t count = std::min(available, bytes.size());
            buffer_.insert(buffer_.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(count));
            bytes = bytes.subspan(count);
        }
    }

    bool writeFromReader(
        const PacketPayloadReader& reader,
        std::size_t offset,
        std::size_t size) {
        while (size > 0) {
            const std::size_t available = capacity_ - buffer_.size();
            if (available == 0) {
                flush();
                continue;
            }

            const std::size_t count = std::min(available, size);
            const std::size_t previousSize = buffer_.size();
            buffer_.resize(previousSize + count);
            const auto readStartedAt = std::chrono::steady_clock::now();
            const bool read = reader.read(
                offset,
                std::span<std::byte>(buffer_.data() + previousSize, count));
            const auto readUs = static_cast<uint64_t>(std::max<int64_t>(
                0,
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - readStartedAt).count()));
            ++sourceReadCalls_;
            sourceReadUs_ += readUs;
            maximumSourceReadUs_ = std::max(maximumSourceReadUs_, readUs);
            if (!read) {
                buffer_.resize(previousSize);
                return false;
            }
            sourceReadBytes_ += count;
            offset += count;
            size -= count;
        }
        return true;
    }

    void flush() {
        if (buffer_.empty()) return;
        prepareForWrite(buffer_.size());
        const auto writeStartedAt = std::chrono::steady_clock::now();
        const bool wrote = out_.write(std::span<const std::byte>(buffer_.data(), buffer_.size()));
        const auto writeDurationUs = static_cast<uint64_t>(std::max<int64_t>(
            0,
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - writeStartedAt).count()));
        ++outputWriteCalls_;
        outputWriteUs_ += writeDurationUs;
        maximumOutputWriteUs_ = std::max(maximumOutputWriteUs_, writeDurationUs);
        if (wrote && adaptiveRateEnabled_) {
            rateController_.observeWrite(buffer_.size(), writeDurationUs);
        }
        buffer_.clear();
        ++flushes_;
    }

    std::size_t flushCount() const {
        return flushes_;
    }

    std::size_t sleepCount() const {
        return sleeps_;
    }

    std::size_t sleepMs() const {
        return sleepMs_;
    }

    std::size_t yieldCount() const { return yields_; }
    std::size_t recoveryWaitCount() const { return recoveryWaits_; }
    std::size_t recoveryWaitMs() const { return recoveryWaitMs_; }
    bool adaptiveRateEnabled() const { return adaptiveRateEnabled_; }
    uint64_t rateLimitBytesPerSecond() const {
        return adaptiveRateEnabled_ ? rateController_.currentBytesPerSecond() : 0;
    }
    uint64_t minimumRateBytesPerSecond() const {
        return adaptiveRateEnabled_ ? rateController_.minimumRateSeen() : 0;
    }
    uint64_t maximumRateBytesPerSecond() const {
        return adaptiveRateEnabled_ ? rateController_.maximumRateSeen() : 0;
    }
    uint64_t observedWriteBytesPerSecond() const {
        return adaptiveRateEnabled_ ? rateController_.observedServiceBytesPerSecond() : 0;
    }
    std::size_t rateAdjustments() const {
        return adaptiveRateEnabled_ ? rateController_.rateAdjustments() : 0;
    }
    std::size_t pressureBackoffs() const {
        return adaptiveRateEnabled_ ? rateController_.pressureBackoffs() : 0;
    }
    std::size_t measuredWrites() const {
        return adaptiveRateEnabled_ ? rateController_.measuredWrites() : 0;
    }
    std::size_t adaptiveBackgroundEntries() const { return adaptiveBackgroundEntries_; }
    std::size_t adaptiveBackgroundMs() const {
        uint64_t totalUs = adaptiveBackgroundUs_;
        if (adaptiveBackgroundStarted_) {
            totalUs += steadyNowUs() - adaptiveBackgroundStartedAtUs_;
        }
        return static_cast<std::size_t>(totalUs / 1000);
    }
    uint64_t sourceReadBytes() const { return sourceReadBytes_; }
    std::size_t sourceReadCalls() const { return sourceReadCalls_; }
    std::size_t sourceReadMs() const { return static_cast<std::size_t>(sourceReadUs_ / 1000); }
    uint64_t maximumSourceReadUs() const { return maximumSourceReadUs_; }
    std::size_t outputWriteCalls() const { return outputWriteCalls_; }
    std::size_t outputWriteMs() const { return static_cast<std::size_t>(outputWriteUs_ / 1000); }
    uint64_t maximumOutputWriteUs() const { return maximumOutputWriteUs_; }
    std::size_t rateLimitSleepCount() const { return rateLimitSleeps_; }
    std::size_t rateLimitSleepMs() const { return rateLimitSleepUs_ / 1000; }
    std::size_t pressureTransitions() const { return pressureTransitions_; }
    int64_t maximumQueueAge100ns() const { return maximumQueueAge100ns_; }
    int maximumEncoderQueueDepth() const { return maximumEncoderQueueDepth_; }
    int maximumNvencInFlight() const { return maximumNvencInFlight_; }
    int64_t maximumCaptureGap100ns() const { return maximumCaptureGap100ns_; }
    int64_t maximumCapturePublicationAge100ns() const { return maximumCapturePublicationAge100ns_; }

private:
    static uint64_t steadyNowUs() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    static AdaptiveWritePressure adaptivePressure(MuxPressureLevel level) {
        switch (level) {
            case MuxPressureLevel::Critical: return AdaptiveWritePressure::Critical;
            case MuxPressureLevel::Elevated: return AdaptiveWritePressure::Elevated;
            default: return AdaptiveWritePressure::Healthy;
        }
    }

    void finishBackgroundMode() {
        if (!adaptiveBackgroundStarted_) return;
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        adaptiveBackgroundUs_ += steadyNowUs() - adaptiveBackgroundStartedAtUs_;
        adaptiveBackgroundStarted_ = false;
    }

    void updateBackgroundMode(AdaptiveWritePressure pressure) {
        if (pressure == AdaptiveWritePressure::Healthy) {
            finishBackgroundMode();
            adaptiveBackgroundAttempted_ = false;
            return;
        }
        if (pressure == AdaptiveWritePressure::Elevated) return;
        if (adaptiveBackgroundStarted_ || adaptiveBackgroundAttempted_) return;
        adaptiveBackgroundAttempted_ = true;
        if (SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != FALSE) {
            adaptiveBackgroundStarted_ = true;
            adaptiveBackgroundStartedAtUs_ = steadyNowUs();
            ++adaptiveBackgroundEntries_;
        }
    }

    MuxPressureSample observePressure() {
        const auto sample = samplePressure_();
        maximumQueueAge100ns_ = std::max(maximumQueueAge100ns_, sample.oldestFrameAge100ns);
        maximumEncoderQueueDepth_ = std::max(maximumEncoderQueueDepth_, sample.encoderQueueDepth);
        maximumNvencInFlight_ = std::max(maximumNvencInFlight_, sample.nvencInFlight);
        maximumCaptureGap100ns_ = std::max(maximumCaptureGap100ns_, sample.captureGap100ns);
        maximumCapturePublicationAge100ns_ = std::max(
            maximumCapturePublicationAge100ns_,
            sample.capturePublicationAge100ns);
        if (sample.level != pressure_) {
            pressure_ = sample.level;
            ++pressureTransitions_;
        }
        const auto sustainedPressure = pressureGate_.update(
            adaptivePressure(sample.level),
            steadyNowUs());
        if (adaptiveRateEnabled_) rateController_.observePressure(sustainedPressure);
        updateBackgroundMode(sustainedPressure);
        return sample;
    }

    void paceElevated(std::size_t pendingBytes) {
        elevatedBytesSinceYield_ += pendingBytes;
        if (elevatedBytesSinceYield_ < elevatedYieldIntervalBytes_) return;
        SwitchToThread();
        ++yields_;
        elevatedBytesSinceYield_ %= elevatedYieldIntervalBytes_;
    }

    void prepareForWrite(std::size_t pendingBytes) {
        if (samplePressure_) {
            auto sample = observePressure();
            if (sample.level == MuxPressureLevel::Elevated) {
                paceElevated(pendingBytes);
            } else if (sample.level == MuxPressureLevel::Critical) {
                elevatedBytesSinceYield_ = 0;
                ++recoveryWaits_;
                const auto waitStartedAt = std::chrono::steady_clock::now();
                constexpr auto maximumRecoveryWait = std::chrono::milliseconds(16);
                do {
                    Sleep(1);
                    ++sleeps_;
                    ++sleepMs_;
                    sample = observePressure();
                } while (
                    sample.level == MuxPressureLevel::Critical &&
                    std::chrono::steady_clock::now() - waitStartedAt < maximumRecoveryWait);
                recoveryWaitMs_ += static_cast<std::size_t>(std::max<int64_t>(
                    0,
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - waitStartedAt).count()));
                if (sample.level == MuxPressureLevel::Elevated) {
                    paceElevated(pendingBytes);
                }
            } else {
                elevatedBytesSinceYield_ = 0;
            }
        }
        paceToTarget(pendingBytes);
    }

    void paceToTarget(std::size_t pendingBytes) {
        const uint64_t bytesPerSecond = rateLimitBytesPerSecond();
        if (bytesPerSecond == 0 || pendingBytes == 0) return;

        using Clock = std::chrono::steady_clock;
        const auto now = Clock::now();
        const auto burstDuration = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(
                static_cast<double>(burstBytes_) / static_cast<double>(bytesPerSecond)));
        const auto writeDuration = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(
                static_cast<double>(pendingBytes) / static_cast<double>(bytesPerSecond)));

        if (!rateLimitStarted_) {
            nextRateLimitedWrite_ = now - burstDuration;
            rateLimitStarted_ = true;
        } else {
            nextRateLimitedWrite_ = std::max(nextRateLimitedWrite_, now - burstDuration);
        }
        nextRateLimitedWrite_ += writeDuration;
        if (nextRateLimitedWrite_ <= now) return;

        const auto sleepStartedAt = Clock::now();
        std::this_thread::sleep_until(nextRateLimitedWrite_);
        const auto sleptUs = std::max<int64_t>(
            0,
            std::chrono::duration_cast<std::chrono::microseconds>(
                Clock::now() - sleepStartedAt).count());
        ++rateLimitSleeps_;
        rateLimitSleepUs_ += static_cast<std::size_t>(sleptUs);
    }

    Win32FileWriter& out_;
    const std::size_t capacity_;
    std::function<MuxPressureSample()> samplePressure_;
    const bool adaptiveRateEnabled_;
    AdaptiveWriteRateController rateController_;
    SustainedWritePressureGate pressureGate_;
    const std::size_t burstBytes_;
    std::vector<std::byte> buffer_;
    std::size_t flushes_ = 0;
    std::size_t sleeps_ = 0;
    std::size_t sleepMs_ = 0;
    std::size_t yields_ = 0;
    std::size_t recoveryWaits_ = 0;
    std::size_t recoveryWaitMs_ = 0;
    std::size_t rateLimitSleeps_ = 0;
    std::size_t rateLimitSleepUs_ = 0;
    std::size_t pressureTransitions_ = 0;
    std::size_t adaptiveBackgroundEntries_ = 0;
    uint64_t adaptiveBackgroundUs_ = 0;
    uint64_t adaptiveBackgroundStartedAtUs_ = 0;
    uint64_t sourceReadBytes_ = 0;
    uint64_t sourceReadUs_ = 0;
    uint64_t maximumSourceReadUs_ = 0;
    uint64_t outputWriteUs_ = 0;
    uint64_t maximumOutputWriteUs_ = 0;
    std::size_t sourceReadCalls_ = 0;
    std::size_t outputWriteCalls_ = 0;
    int64_t maximumQueueAge100ns_ = 0;
    int maximumEncoderQueueDepth_ = 0;
    int maximumNvencInFlight_ = 0;
    int64_t maximumCaptureGap100ns_ = 0;
    int64_t maximumCapturePublicationAge100ns_ = 0;
    MuxPressureLevel pressure_ = MuxPressureLevel::Healthy;
    bool adaptiveBackgroundStarted_ = false;
    bool adaptiveBackgroundAttempted_ = false;
    bool rateLimitStarted_ = false;
    std::chrono::steady_clock::time_point nextRateLimitedWrite_ {};
    std::size_t elevatedBytesSinceYield_ = 0;
    static constexpr std::size_t elevatedYieldIntervalBytes_ = 16u * 1024u * 1024u;
};

void writeU32(BufferedByteWriter& out, uint32_t value) {
    const std::byte bytes[4] = {
        static_cast<std::byte>((value >> 24) & 0xFF),
        static_cast<std::byte>((value >> 16) & 0xFF),
        static_cast<std::byte>((value >> 8) & 0xFF),
        static_cast<std::byte>(value & 0xFF)
    };
    out.write(std::span<const std::byte>(bytes, 4));
}

void writeBytes(BufferedByteWriter& out, std::span<const std::byte> bytes) {
    out.write(bytes);
}

void writeBytes(BufferedByteWriter& out, const Bytes& bytes) {
    writeBytes(out, std::as_bytes(std::span<const uint8_t>(bytes.data(), bytes.size())));
}

void writeBytes(BufferedByteWriter& out, const std::vector<std::byte>& bytes) {
    writeBytes(out, std::span<const std::byte>(bytes.data(), bytes.size()));
}

bool writePacketRange(
    BufferedByteWriter& out,
    const EncodedPacket& packet,
    std::size_t offset,
    std::size_t size) {
    if (offset > payloadSize(packet) || size > payloadSize(packet) - offset) return false;
    if (size == 0) return true;
    if (packet.payload) {
        writeBytes(out, std::span<const std::byte>(packet.payload->data() + offset, size));
        return true;
    }
    if (!packet.payloadReader) return false;
    return out.writeFromReader(*packet.payloadReader, offset, size);
}

bool writeSamplePayload(
    BufferedByteWriter& out,
    const OwnedSample& sample) {
    const auto memory = samplePayload(sample);
    if (!memory.empty()) {
        writeBytes(out, memory);
        return true;
    }
    if (!sample.payloadReader) return sample.info.size == 0;
    return out.writeFromReader(*sample.payloadReader, 0, sample.info.size);
}

bool writeAvccSample(
    BufferedByteWriter& out,
    const VideoSamplePlan& sample) {
    if (!sample.packet) return false;
    for (const auto& nalu : sample.writableNalus) {
        writeU32(out, static_cast<uint32_t>(nalu.size));
        if (!writePacketRange(out, *sample.packet, nalu.offset, nalu.size)) return false;
    }
    return true;
}

}  // namespace

MuxResult muxH264ToMp4(
    const std::vector<EncodedPacket>& packets,
    const std::string& saveFolder,
    int width,
    int height,
    int fps,
    int /*bitrateMbps*/,
    MuxWritePacing pacing) {
    MuxResult result;
    const auto totalStartedAt = SaveTimingClock::now();
    logMuxSaveTiming(
        "start",
        totalStartedAt,
        "packets=" + std::to_string(packets.size()) +
            " resolution=\"" + std::to_string(width) + "x" + std::to_string(height) + "\"" +
            " fps=" + std::to_string(fps));

    bool hasVideoPacket = false;
    std::vector<VideoSamplePlan> videoSamples;
    std::vector<PcmAudioTrack> pcmAudioTracks;
    std::vector<AacAudioTrack> audioTracks;
    std::vector<std::byte> sps;
    std::vector<std::byte> pps;
    std::size_t videoPacketCount = 0;
    std::size_t pcmPacketCount = 0;
    std::size_t aacPacketCount = 0;
    std::size_t writableNaluCount = 0;
    uint64_t videoSourceBytes = 0;
    uint64_t pcmSourceBytes = 0;

    const auto prepassStartedAt = SaveTimingClock::now();
    for (const auto& packet : packets) {
        if (isVideoPacket(packet)) {
            hasVideoPacket = true;
            ++videoPacketCount;
            videoSourceBytes += payloadSize(packet);
            VideoSamplePlan sample;
            sample.packet = &packet;
            sample.info.keyframe = packet.keyframe;
            auto collectNalu = [&](const NalUnit& nalu) {
                if (nalu.type == 7 && sps.empty()) {
                    sps = copyNaluPayload(packet, nalu);
                } else if (nalu.type == 8 && pps.empty()) {
                    pps = copyNaluPayload(packet, nalu);
                }

                if (isWritableVideoNalu(nalu)) {
                    sample.writableNalus.push_back(nalu);
                }
            };

            if (packet.h264.analyzed) {
                forEachH264Nal(packet.h264, collectNalu);
                sample.info.size = packet.h264.avccSampleSize;
            } else {
                std::vector<std::byte> materialized;
                if (copyPayloadRange(packet, 0, payloadSize(packet), materialized)) {
                    const auto nalus = parseAnnexBNalus(materialized);
                    for (const auto& nalu : nalus) collectNalu(nalu);
                    sample.info.size = avccSampleSize(sample.writableNalus);
                }
            }
            if (sample.info.size > 0) {
                writableNaluCount += sample.writableNalus.size();
                videoSamples.push_back(std::move(sample));
            }
        } else if (isAacAudioPacket(packet)) {
            ++aacPacketCount;
            const auto& trackId = packetTrackId(packet);
            auto track = std::find_if(audioTracks.begin(), audioTracks.end(), [&](const AacAudioTrack& candidate) {
                return candidate.sourceId == trackId &&
                    candidate.sampleRate == packet.sampleRate &&
                    candidate.channels == packet.channelCount;
            });
            if (track == audioTracks.end()) {
                audioTracks.push_back(AacAudioTrack {
                    trackId,
                    packet.sampleRate,
                    packet.channelCount,
                    makeAacAudioSpecificConfig(packet.sampleRate, packet.channelCount),
                    {},
                    packet.pts100ns
                });
                track = std::prev(audioTracks.end());
            }
            OwnedSample sample;
            sample.sharedPayload = packet.payload;
            sample.payloadReader = packet.payloadReader;
            sample.info.size = payloadSize(packet);
            sample.info.duration = packet.audioFrameCount;
            sample.pts100ns = packet.pts100ns;
            sample.encoderEpoch = packet.encoderEpoch;
            sample.primingFrames = packet.audioPrimingFrames;
            sample.encodedFrameCount = packet.audioFrameCount;
            track->samples.push_back(std::move(sample));
            if (track->firstPts100ns == 0 || packet.pts100ns < track->firstPts100ns) {
                track->firstPts100ns = packet.pts100ns;
            }
        } else if (isPcmAudioPacket(packet)) {
            ++pcmPacketCount;
            const auto& trackId = packetTrackId(packet);
            auto track = std::find_if(pcmAudioTracks.begin(), pcmAudioTracks.end(), [&](const PcmAudioTrack& candidate) {
                return candidate.sourceId == trackId &&
                    candidate.sampleRate == packet.sampleRate &&
                    candidate.channels == packet.channelCount;
            });
            if (track == pcmAudioTracks.end()) {
                pcmAudioTracks.push_back(PcmAudioTrack {
                    trackId,
                    packet.sampleRate,
                    packet.channelCount,
                    {}
                });
                track = std::prev(pcmAudioTracks.end());
            }

            const auto bytes = payloadBytes(packet);
            pcmSourceBytes += bytes.size();
            const auto bytesPerFrame = static_cast<std::size_t>(std::max(1, packet.channelCount) * 2);
            const auto frames = static_cast<uint32_t>(std::max<std::size_t>(1, bytes.size() / bytesPerFrame));
            track->samples.push_back(PcmSampleView { bytes, packet.pts100ns, frames });
        }
    }
    logMuxSaveTiming(
        "prepass",
        prepassStartedAt,
        "videoPackets=" + std::to_string(videoPacketCount) +
            " videoSamples=" + std::to_string(videoSamples.size()) +
            " writableNalus=" + std::to_string(writableNaluCount) +
            " videoBytes=" + std::to_string(videoSourceBytes) +
            " pcmPackets=" + std::to_string(pcmPacketCount) +
            " pcmTracks=" + std::to_string(pcmAudioTracks.size()) +
            " pcmBytes=" + std::to_string(pcmSourceBytes) +
            " aacPackets=" + std::to_string(aacPacketCount) +
            " aacTracks=" + std::to_string(audioTracks.size()));
    if (!hasVideoPacket) {
        result.message = "No encoded H.264 packets are buffered yet.";
        logMuxSaveTiming("total", totalStartedAt, "ok=false reason=no_video_packets");
        return result;
    }

    const auto avcConfig = buildAvcDecoderConfig(sps, pps);
    if (avcConfig.empty()) {
        result.message = "MP4 muxing failed: the selected H.264 window has no SPS/PPS decoder header yet.";
        logMuxSaveTiming("total", totalStartedAt, "ok=false reason=missing_avc_config");
        return result;
    }

    // Compute per-frame durations from PTS gaps (timescale = 10,000,000 = 100ns units).
    // Previously each frame had duration=1 at timescale=fps, which assumed exactly
    // fps*seconds frames were captured. In practice WGC delivers frames at variable
    // rates and the encoder throttle drops some, so fewer frames exist, causing speedup.
    const auto durationStartedAt = SaveTimingClock::now();
    if (videoSamples.size() >= 2) {
        for (std::size_t i = 0; i + 1 < videoSamples.size(); ++i) {
            const int64_t gap = std::max<int64_t>(1, videoSamples[i + 1].packet->pts100ns - videoSamples[i].packet->pts100ns);
            videoSamples[i].info.duration = static_cast<uint32_t>(std::min<int64_t>(gap, 0xFFFFFFFFLL));
        }
        // A large gap is already represented by the preceding sample. Reusing the
        // average here would count that gap a second time at the end of the clip.
        videoSamples.back().info.duration = finalVideoSampleDuration100ns(
            videoSamples.back().packet->duration100ns, fps);
    } else if (videoSamples.size() == 1) {
        videoSamples[0].info.duration = static_cast<uint32_t>(10'000'000LL / std::max(1, fps));
    }
    if (videoSamples.empty()) {
        result.message = "MP4 muxing failed: the selected H.264 window contains no writable frame samples.";
        logMuxSaveTiming("total", totalStartedAt, "ok=false reason=no_writable_samples");
        return result;
    }
    const uint64_t plannedVideoDuration100ns = samplesDuration(videoSamples);
    const int64_t videoMediaStartPts100ns = videoSamples.front().packet
        ? videoSamples.front().packet->pts100ns
        : 0;
    const int64_t boundedMediaDuration100ns = static_cast<int64_t>(std::min<uint64_t>(
        plannedVideoDuration100ns,
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max())));
    const int64_t videoMediaEndPts100ns = videoMediaStartPts100ns >
            std::numeric_limits<int64_t>::max() - boundedMediaDuration100ns
        ? std::numeric_limits<int64_t>::max()
        : videoMediaStartPts100ns + boundedMediaDuration100ns;
    int64_t videoPresentationStartPts100ns = pacing.presentationStartPts100ns > 0
        ? std::clamp(
            pacing.presentationStartPts100ns,
            videoMediaStartPts100ns,
            videoMediaEndPts100ns)
        : videoMediaStartPts100ns;
    int64_t videoPresentationEndPts100ns = pacing.presentationEndPts100ns > videoPresentationStartPts100ns
        ? std::clamp(
            pacing.presentationEndPts100ns,
            videoPresentationStartPts100ns,
            videoMediaEndPts100ns)
        : videoMediaEndPts100ns;
    if (videoPresentationEndPts100ns <= videoPresentationStartPts100ns) {
        videoPresentationStartPts100ns = videoMediaStartPts100ns;
        videoPresentationEndPts100ns = videoMediaEndPts100ns;
    }
    const uint64_t movieDuration100ns = static_cast<uint64_t>(
        videoPresentationEndPts100ns - videoPresentationStartPts100ns);
    const uint64_t videoMediaStart100ns = static_cast<uint64_t>(
        videoPresentationStartPts100ns - videoMediaStartPts100ns);
    const uint32_t targetFrameDuration100ns = static_cast<uint32_t>(10'000'000LL / std::max(1, fps));
    uint32_t maxSampleDuration100ns = 0;
    std::size_t gapEvents = 0;
    uint64_t missingFrameSlots = 0;
    for (const auto& sample : videoSamples) {
        maxSampleDuration100ns = std::max(maxSampleDuration100ns, sample.info.duration);
        const uint64_t roundedTicks =
            (static_cast<uint64_t>(sample.info.duration) + targetFrameDuration100ns / 2u) /
            targetFrameDuration100ns;
        const uint64_t missing = roundedTicks > 1 ? roundedTicks - 1 : 0;
        if (missing > 0) {
            ++gapEvents;
            missingFrameSlots += missing;
        }
    }
    logMuxSaveTiming(
        "duration_plan",
        durationStartedAt,
        "videoSamples=" + std::to_string(videoSamples.size()) +
            " plannedDuration100ns=" + std::to_string(plannedVideoDuration100ns) +
            " presentationDuration100ns=" + std::to_string(movieDuration100ns) +
            " decoderPreroll100ns=" + std::to_string(videoMediaStart100ns) +
            " maxSampleDuration100ns=" + std::to_string(maxSampleDuration100ns) +
            " gapEvents=" + std::to_string(gapEvents) +
            " missingFrameSlots=" + std::to_string(missingFrameSlots));

    if (!pcmAudioTracks.empty()) {
        const auto aacStartedAt = SaveTimingClock::now();
        std::string audioError;
        for (const auto& pcmTrack : pcmAudioTracks) {
            const auto trackStartedAt = SaveTimingClock::now();
            auto existing = std::find_if(audioTracks.begin(), audioTracks.end(), [&](const AacAudioTrack& candidate) {
                return candidate.sourceId == pcmTrack.sourceId &&
                    candidate.sampleRate == pcmTrack.sampleRate &&
                    candidate.channels == pcmTrack.channels;
            });
            if (existing != audioTracks.end()) finalizeAudioTimeline(*existing);
            const auto missingPcm = selectUncoveredPcm(
                pcmTrack,
                existing == audioTracks.end() ? nullptr : &*existing);
            AacAudioTrack recoveredTrack;
            const bool encoded = missingPcm.samples.empty()
                ? true
                : encodePcmTrackToAacBatched(missingPcm, recoveredTrack, audioError);
            logMuxSaveTiming(
                "aac_encode_track",
                trackStartedAt,
                "ok=" + std::string(encoded ? "true" : "false") +
                    " source=\"" + pcmTrack.sourceId + "\"" +
                    " pcmSamples=" + std::to_string(pcmTrack.samples.size()) +
                    " repairSamples=" + std::to_string(missingPcm.samples.size()) +
                    " aacSamples=" + std::to_string(recoveredTrack.samples.size()));
            if (encoded && !recoveredTrack.samples.empty()) {
                if (existing == audioTracks.end()) {
                    audioTracks.push_back(std::move(recoveredTrack));
                } else {
                    existing->samples.insert(
                        existing->samples.end(),
                        std::make_move_iterator(recoveredTrack.samples.begin()),
                        std::make_move_iterator(recoveredTrack.samples.end()));
                }
            }
        }
        logMuxSaveTiming(
            "aac_encode",
            aacStartedAt,
            "pcmTracks=" + std::to_string(pcmAudioTracks.size()) +
                " aacTracks=" + std::to_string(audioTracks.size()));

        if (audioTracks.empty() && !pcmAudioTracks.empty()) {
            result.message = audioError.empty()
                ? "AAC audio encoding failed for every captured audio track."
                : audioError;
            logMuxSaveTiming("total", totalStartedAt, "ok=false reason=aac_encode_failed");
            return result;
        }
    }

    AudioContinuityStats audioContinuity;
    AudioClipAlignmentStats audioAlignment;
    std::string audioContinuityError;
    for (auto& track : audioTracks) {
        finalizeAudioTimeline(track);
        if (!makeAudioTimelineContinuous(track, audioContinuity, audioContinuityError)) {
            result.message = audioContinuityError.empty()
                ? "AAC timeline normalization failed."
                : audioContinuityError;
            logMuxSaveTiming("total", totalStartedAt, "ok=false reason=aac_timeline_normalization");
            return result;
        }
        alignAudioTrackToVideo(
            track,
            videoPresentationStartPts100ns,
            movieDuration100ns,
            audioAlignment);
    }
    logMuxSaveTiming(
        "audio_timeline",
        totalStartedAt,
        "tracks=" + std::to_string(audioTracks.size()) +
            " insertedSilenceSamples=" + std::to_string(audioContinuity.insertedSilenceSamples) +
            " removedOverlaps=" + std::to_string(audioContinuity.removedOverlappingSamples) +
            " maximumGap100ns=" + std::to_string(audioContinuity.maximumGap100ns) +
            " droppedLeadingSamples=" + std::to_string(audioAlignment.droppedLeadingSamples) +
            " droppedLeadingFrames=" + std::to_string(audioAlignment.droppedLeadingFrames) +
            " droppedTrailingSamples=" + std::to_string(audioAlignment.droppedTrailingSamples) +
            " maximumStartDelay100ns=" + std::to_string(audioAlignment.maximumStartDelay100ns));
    std::erase_if(audioTracks, [](const AacAudioTrack& track) { return track.samples.empty(); });

    videoSamples.front().info.keyframe = true;

    const auto metadataStartedAt = SaveTimingClock::now();
    const auto path = clipFilePath(saveFolder);
    const auto ftyp = makeFtyp();
    uint64_t mdatPayloadSize = 0;
    for (const auto& sample : videoSamples) mdatPayloadSize += sample.info.size;
    for (const auto& track : audioTracks) {
        for (const auto& sample : track.samples) mdatPayloadSize += sample.info.size;
    }

    const bool largeMdat = mdatPayloadSize + 8 > 0xFFFFFFFFULL;
    const uint64_t mdatHeaderSize = largeMdat ? 16 : 8;
    uint64_t nextOffset = ftyp.size() + mdatHeaderSize;
    for (auto& sample : videoSamples) {
        sample.info.fileOffset = nextOffset;
        nextOffset += sample.info.size;
    }
    for (auto& track : audioTracks) {
        for (auto& sample : track.samples) {
            sample.info.fileOffset = nextOffset;
            nextOffset += sample.info.size;
        }
    }

    const auto moov = makeMoov(
        avcConfig,
        videoSamples,
        audioTracks,
        width,
        height,
        fps,
        movieDuration100ns,
        videoMediaStart100ns,
        videoPresentationStartPts100ns);
    logMuxSaveTiming(
        "metadata",
        metadataStartedAt,
        "mdatBytes=" + std::to_string(mdatPayloadSize) +
            " moovBytes=" + std::to_string(moov.size()) +
            " audioTracks=" + std::to_string(audioTracks.size()));

    Win32FileWriter out(path);
    if (!out.valid()) {
        result.message = "MP4 muxing failed: could not create output file.";
        logMuxSaveTiming("total", totalStartedAt, "ok=false reason=create_output_failed");
        return result;
    }
    const uint64_t finalFileSize = ftyp.size() + mdatHeaderSize + mdatPayloadSize + moov.size();
    const bool preallocated = out.preallocate(finalFileSize);
    const bool adaptiveWritePacing = shouldUseAdaptiveWritePacing(
        pacing.storageAwareRate,
        out.storageSeekPenalty());
    const auto adaptiveRateConfig = writePacerConfigForStorage(
        pacing.adaptiveRate,
        out.storageSeekPenalty());

    const auto headerStartedAt = SaveTimingClock::now();
    writeBytes(out, ftyp);
    if (largeMdat) {
        writeU32(out, 1);
        writeType(out, "mdat");
        writeU64(out, mdatPayloadSize + 16);
    } else {
        writeU32(out, static_cast<uint32_t>(mdatPayloadSize + 8));
        writeType(out, "mdat");
    }
    logMuxSaveTiming(
        "write_header",
        headerStartedAt,
        "largeMdat=" + std::string(largeMdat ? "true" : "false") +
            " preallocated=" + std::string(preallocated ? "true" : "false") +
            " finalBytes=" + std::to_string(finalFileSize) +
            " lowIoPriority=" + std::string(out.lowPriorityApplied() ? "true" : "false") +
            " ioPriority=" + ioPriorityName(out.ioPriorityHint()) +
            " storageSeekPenalty=" + storageSeekPenaltyName(out.storageSeekPenalty()) +
            " adaptiveWritePacing=" + std::string(adaptiveWritePacing ? "true" : "false") +
            " initialRateMiBps=" + std::to_string(
                adaptiveWritePacing
                    ? adaptiveRateConfig.initialBytesPerSecond / (1024ULL * 1024ULL)
                    : 0));

    constexpr std::size_t liveSaveChunkBytes = 512u * 1024u;
    BufferedByteWriter bufferedOut(
        out,
        liveSaveChunkBytes,
        std::move(pacing.samplePressure),
        adaptiveWritePacing,
        adaptiveRateConfig,
        pacing.burstBytes);
    const auto videoWriteStartedAt = SaveTimingClock::now();
    uint64_t videoWrittenBytes = 0;
    bool payloadReadSucceeded = true;
    for (const auto& sample : videoSamples) {
        if (!writeAvccSample(bufferedOut, sample)) {
            payloadReadSucceeded = false;
            break;
        }
        videoWrittenBytes += sample.info.size;
    }
    bufferedOut.flush();
    const auto videoFlushes = bufferedOut.flushCount();
    const auto videoSleeps = bufferedOut.sleepCount();
    const auto videoSleepMs = bufferedOut.sleepMs();
    const int64_t videoWriteMs = std::max<int64_t>(1, saveTimingElapsedMs(videoWriteStartedAt));
    const uint64_t videoThroughputMiBps =
        (videoWrittenBytes * 1000ULL) / (static_cast<uint64_t>(videoWriteMs) * 1024ULL * 1024ULL);
    logMuxSaveTiming(
        "write_video_mdat",
        videoWriteStartedAt,
        "samples=" + std::to_string(videoSamples.size()) +
            " bytes=" + std::to_string(videoWrittenBytes) +
            " throughputMiBps=" + std::to_string(videoThroughputMiBps) +
            " flushes=" + std::to_string(videoFlushes) +
            " maxWriteBytes=" + std::to_string(out.maximumWriteSize()) +
            " sourceReadBytes=" + std::to_string(bufferedOut.sourceReadBytes()) +
            " sourceReadCalls=" + std::to_string(bufferedOut.sourceReadCalls()) +
            " sourceReadMs=" + std::to_string(bufferedOut.sourceReadMs()) +
            " maxSourceReadUs=" + std::to_string(bufferedOut.maximumSourceReadUs()) +
            " outputWriteCalls=" + std::to_string(bufferedOut.outputWriteCalls()) +
            " outputWriteMs=" + std::to_string(bufferedOut.outputWriteMs()) +
            " maxOutputWriteUs=" + std::to_string(bufferedOut.maximumOutputWriteUs()) +
            " adaptiveBackgroundEntries=" + std::to_string(
                bufferedOut.adaptiveBackgroundEntries()) +
            " adaptiveBackgroundMs=" + std::to_string(bufferedOut.adaptiveBackgroundMs()) +
            " adaptiveRate=" + std::string(
                bufferedOut.adaptiveRateEnabled() ? "true" : "false") +
            " finalRateMiBps=" + std::to_string(
                bufferedOut.rateLimitBytesPerSecond() / (1024ULL * 1024ULL)) +
            " minimumRateMiBps=" + std::to_string(
                bufferedOut.minimumRateBytesPerSecond() / (1024ULL * 1024ULL)) +
            " maximumRateMiBps=" + std::to_string(
                bufferedOut.maximumRateBytesPerSecond() / (1024ULL * 1024ULL)) +
            " observedWriteMiBps=" + std::to_string(
                bufferedOut.observedWriteBytesPerSecond() / (1024ULL * 1024ULL)) +
            " measuredWrites=" + std::to_string(bufferedOut.measuredWrites()) +
            " rateAdjustments=" + std::to_string(bufferedOut.rateAdjustments()) +
            " pressureBackoffs=" + std::to_string(bufferedOut.pressureBackoffs()) +
            " rateLimitSleeps=" + std::to_string(bufferedOut.rateLimitSleepCount()) +
            " rateLimitSleepMs=" + std::to_string(bufferedOut.rateLimitSleepMs()) +
            " paceYields=" + std::to_string(bufferedOut.yieldCount()) +
            " paceSleeps=" + std::to_string(videoSleeps) +
            " paceSleepMs=" + std::to_string(videoSleepMs) +
            " recoveryWaits=" + std::to_string(bufferedOut.recoveryWaitCount()) +
            " recoveryWaitMs=" + std::to_string(bufferedOut.recoveryWaitMs()) +
            " pressureTransitions=" + std::to_string(bufferedOut.pressureTransitions()) +
            " maxQueueAge100ns=" + std::to_string(bufferedOut.maximumQueueAge100ns()) +
            " maxEncoderQueueDepth=" + std::to_string(bufferedOut.maximumEncoderQueueDepth()) +
            " maxNvencInFlight=" + std::to_string(bufferedOut.maximumNvencInFlight()) +
            " maxCaptureGap100ns=" + std::to_string(bufferedOut.maximumCaptureGap100ns()) +
            " maxCapturePublicationAge100ns=" + std::to_string(
                bufferedOut.maximumCapturePublicationAge100ns()));

    const auto audioWriteStartedAt = SaveTimingClock::now();
    const auto audioFlushesBefore = bufferedOut.flushCount();
    const auto audioSleepsBefore = bufferedOut.sleepCount();
    const auto audioSleepMsBefore = bufferedOut.sleepMs();
    const auto audioRateLimitSleepsBefore = bufferedOut.rateLimitSleepCount();
    const auto audioRateLimitSleepMsBefore = bufferedOut.rateLimitSleepMs();
    const auto audioSourceReadBytesBefore = bufferedOut.sourceReadBytes();
    const auto audioSourceReadCallsBefore = bufferedOut.sourceReadCalls();
    const auto audioSourceReadMsBefore = bufferedOut.sourceReadMs();
    const auto audioOutputWriteCallsBefore = bufferedOut.outputWriteCalls();
    const auto audioOutputWriteMsBefore = bufferedOut.outputWriteMs();
    uint64_t audioWrittenBytes = 0;
    for (const auto& track : audioTracks) {
        if (!payloadReadSucceeded) break;
        for (const auto& sample : track.samples) {
            if (!writeSamplePayload(bufferedOut, sample)) {
                payloadReadSucceeded = false;
                break;
            }
            audioWrittenBytes += sample.info.size;
        }
    }
    bufferedOut.flush();
    logMuxSaveTiming(
        "write_audio_mdat",
        audioWriteStartedAt,
        "tracks=" + std::to_string(audioTracks.size()) +
            " bytes=" + std::to_string(audioWrittenBytes) +
            " flushes=" + std::to_string(bufferedOut.flushCount() - audioFlushesBefore) +
            " sourceReadBytes=" + std::to_string(
                bufferedOut.sourceReadBytes() - audioSourceReadBytesBefore) +
            " sourceReadCalls=" + std::to_string(
                bufferedOut.sourceReadCalls() - audioSourceReadCallsBefore) +
            " sourceReadMs=" + std::to_string(
                bufferedOut.sourceReadMs() - audioSourceReadMsBefore) +
            " outputWriteCalls=" + std::to_string(
                bufferedOut.outputWriteCalls() - audioOutputWriteCallsBefore) +
            " outputWriteMs=" + std::to_string(
                bufferedOut.outputWriteMs() - audioOutputWriteMsBefore) +
            " rateLimitSleeps=" + std::to_string(
                bufferedOut.rateLimitSleepCount() - audioRateLimitSleepsBefore) +
            " rateLimitSleepMs=" + std::to_string(
                bufferedOut.rateLimitSleepMs() - audioRateLimitSleepMsBefore) +
            " paceSleeps=" + std::to_string(bufferedOut.sleepCount() - audioSleepsBefore) +
            " paceSleepMs=" + std::to_string(bufferedOut.sleepMs() - audioSleepMsBefore));

    const auto moovWriteStartedAt = SaveTimingClock::now();
    writeBytes(out, moov);
    logMuxSaveTiming("write_moov", moovWriteStartedAt, "bytes=" + std::to_string(moov.size()));

    const auto closeStartedAt = SaveTimingClock::now();
    const bool writeSucceeded = payloadReadSucceeded && out.good();
    const bool closeSucceeded = out.close();
    logMuxSaveTiming("file_close", closeStartedAt);

    if (!writeSucceeded || !closeSucceeded) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        result.message = payloadReadSucceeded
            ? "MP4 muxing failed while writing the output file."
            : "MP4 muxing failed while reading the replay archive.";
        logMuxSaveTiming(
            "total",
            totalStartedAt,
            std::string("ok=false reason=") + (payloadReadSucceeded ? "write_failed" : "archive_read_failed"));
        return result;
    }

    result.ok = true;
    result.filePath = narrow(path);
    result.message = "Saved MP4 clip.";
    logMuxSaveTiming(
        "total",
        totalStartedAt,
        "ok=true videoBytes=" + std::to_string(videoWrittenBytes) +
            " audioBytes=" + std::to_string(audioWrittenBytes) +
            " fileBytes=" + std::to_string(out.bytesWritten()) +
            " maxWriteBytes=" + std::to_string(out.maximumWriteSize()) +
            " path=\"" + result.filePath + "\"");
    return result;
}

}  // namespace clipture
