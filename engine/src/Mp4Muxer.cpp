#include "clipture/Mp4Muxer.hpp"
#include "clipture/AacEncoderSession.hpp"
#include "clipture/BoundedWrite.hpp"

#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <sstream>
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

std::vector<std::byte> copyNaluPayload(std::span<const std::byte> data, const NalUnit& nalu) {
    if (nalu.offset + nalu.size > data.size()) return {};
    return {
        data.begin() + static_cast<std::ptrdiff_t>(nalu.offset),
        data.begin() + static_cast<std::ptrdiff_t>(nalu.offset + nalu.size)
    };
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

    struct AudioRun {
        int64_t presentationStart100ns = 0;
        uint64_t mediaStartFrames = 0;
        uint64_t mediaDurationFrames = 0;
        uint32_t primingFrames = 0;
        uint32_t epoch = 0;
    };

    constexpr int64_t kContiguousTolerance100ns = 50'000LL;
    const int sampleRate = std::max(1, track.sampleRate);
    std::vector<AudioRun> runs;
    uint64_t mediaCursorFrames = 0;
    int64_t expectedNextPts100ns = 0;
    for (const auto& sample : track.samples) {
        const uint32_t frameCount = std::max<uint32_t>(1, sample.encodedFrameCount);
        const bool startsRun = runs.empty() ||
            sample.encoderEpoch != runs.back().epoch ||
            std::abs(sample.pts100ns - expectedNextPts100ns) > kContiguousTolerance100ns;
        if (startsRun) {
            runs.push_back(AudioRun {
                sample.pts100ns,
                mediaCursorFrames,
                0,
                static_cast<uint32_t>(std::max<int32_t>(0, sample.primingFrames)),
                sample.encoderEpoch
            });
        }
        runs.back().mediaDurationFrames += frameCount;
        mediaCursorFrames += frameCount;
        expectedNextPts100ns = sample.pts100ns +
            (static_cast<int64_t>(frameCount) * 10'000'000LL) / sampleRate;
    }

    struct EditEntry {
        uint64_t duration100ns = 0;
        int64_t mediaTimeFrames = -1;
    };
    std::vector<EditEntry> edits;
    uint64_t presentationCursor100ns = 0;
    for (const auto& run : runs) {
        const int64_t relativeStart100ns = run.presentationStart100ns - videoStartPts100ns;
        uint64_t desiredStart100ns = relativeStart100ns > 0
            ? static_cast<uint64_t>(relativeStart100ns)
            : 0;
        desiredStart100ns = std::min(desiredStart100ns, movieDuration100ns);
        if (desiredStart100ns > presentationCursor100ns) {
            edits.push_back({ desiredStart100ns - presentationCursor100ns, -1 });
            presentationCursor100ns = desiredStart100ns;
        }
        if (presentationCursor100ns >= movieDuration100ns) break;

        uint64_t trimmedFrames = run.primingFrames;
        if (relativeStart100ns < 0) {
            trimmedFrames += static_cast<uint64_t>(
                ((-relativeStart100ns) * sampleRate + 9'999'999LL) / 10'000'000LL);
        } else if (desiredStart100ns < presentationCursor100ns) {
            const uint64_t overlap100ns = presentationCursor100ns - desiredStart100ns;
            trimmedFrames += (overlap100ns * static_cast<uint64_t>(sampleRate) + 9'999'999ULL) /
                10'000'000ULL;
        }
        if (trimmedFrames >= run.mediaDurationFrames) continue;

        const uint64_t playableFrames = run.mediaDurationFrames - trimmedFrames;
        uint64_t playableDuration100ns =
            (playableFrames * 10'000'000ULL) / static_cast<uint64_t>(sampleRate);
        playableDuration100ns = std::min(
            playableDuration100ns,
            movieDuration100ns - presentationCursor100ns);
        if (playableDuration100ns == 0) continue;
        edits.push_back({
            playableDuration100ns,
            static_cast<int64_t>(run.mediaStartFrames + trimmedFrames)
        });
        presentationCursor100ns += playableDuration100ns;
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

Bytes makeVideoTrak(const Bytes& avcConfig, const std::vector<VideoSamplePlan>& samples, int width, int height, int /*fps*/) {
    const uint32_t timescale = 10'000'000u; // 100ns units — matches PTS-based sample durations
    const uint64_t duration = samplesDuration(samples);

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
    appendBytes(mdiaPayload, makeMdhd(timescale, duration));
    appendBytes(mdiaPayload, makeHdlr());
    appendBytes(mdiaPayload, box("minf", minfPayload));

    Bytes trakPayload;
    appendBytes(trakPayload, makeTkhd(duration, width, height));
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

Bytes makeMoov(const Bytes& avcConfig, const std::vector<VideoSamplePlan>& samples, const std::vector<AacAudioTrack>& audioTracks, int width, int height, int fps) {
    const uint32_t timescale = 10'000'000u; // movie timescale in 100ns units — matches video track
    const uint64_t duration = samplesDuration(samples);

    Bytes moovPayload;
    appendBytes(moovPayload, makeMvhd(timescale, duration, static_cast<uint32_t>(audioTracks.size() + 2)));
    appendBytes(moovPayload, makeVideoTrak(avcConfig, samples, width, height, fps));
    const int64_t videoStartPts100ns = samples.empty() || !samples.front().packet
        ? 0
        : samples.front().packet->pts100ns;
    uint32_t trackId = 2;
    for (const auto& audioTrack : audioTracks) {
        appendBytes(moovPayload, makeAudioTrak(audioTrack, trackId++, duration, videoStartPts100ns));
    }
    return box("moov", moovPayload);
}

class Win32FileWriter {
public:
    static constexpr DWORD maxWriteBytes = 4u * 1024u * 1024u;

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

        FILE_IO_PRIORITY_HINT_INFO priorityInfo {};
        priorityInfo.PriorityHint = IoPriorityHintLow;
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
        std::function<MuxPressureSample()> samplePressure = {})
        : out_(out),
          capacity_(std::max<std::size_t>(capacity, 1)),
          samplePressure_(std::move(samplePressure)) {
        buffer_.reserve(capacity_);
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

    void flush() {
        if (buffer_.empty()) return;
        out_.write(std::span<const std::byte>(buffer_.data(), buffer_.size()));
        buffer_.clear();
        noteFlush();
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
    std::size_t pressureTransitions() const { return pressureTransitions_; }
    int64_t maximumQueueAge100ns() const { return maximumQueueAge100ns_; }
    int maximumNvencPending() const { return maximumNvencPending_; }
    int64_t maximumCaptureGap100ns() const { return maximumCaptureGap100ns_; }

private:
    void noteFlush() {
        ++flushes_;
        if (!samplePressure_) return;
        const auto sample = samplePressure_();
        maximumQueueAge100ns_ = std::max(maximumQueueAge100ns_, sample.oldestFrameAge100ns);
        maximumNvencPending_ = std::max(maximumNvencPending_, sample.nvencPending);
        maximumCaptureGap100ns_ = std::max(maximumCaptureGap100ns_, sample.captureGap100ns);

        auto requested = sample.level;
        const auto now = std::chrono::steady_clock::now();
        if (requested == MuxPressureLevel::Healthy && pressure_ != MuxPressureLevel::Healthy) {
            if (healthySince_.time_since_epoch().count() == 0) healthySince_ = now;
            if (now - healthySince_ < std::chrono::milliseconds(250)) requested = pressure_;
        } else if (requested != MuxPressureLevel::Healthy) {
            healthySince_ = {};
        }
        if (requested != pressure_) {
            pressure_ = requested;
            ++pressureTransitions_;
        }
        if (pressure_ == MuxPressureLevel::Critical) {
            Sleep(1);
            ++sleeps_;
            ++sleepMs_;
        } else if (pressure_ == MuxPressureLevel::Elevated) {
            SwitchToThread();
            ++yields_;
        }
    }

    Win32FileWriter& out_;
    const std::size_t capacity_;
    std::function<MuxPressureSample()> samplePressure_;
    std::vector<std::byte> buffer_;
    std::size_t flushes_ = 0;
    std::size_t sleeps_ = 0;
    std::size_t sleepMs_ = 0;
    std::size_t yields_ = 0;
    std::size_t pressureTransitions_ = 0;
    int64_t maximumQueueAge100ns_ = 0;
    int maximumNvencPending_ = 0;
    int64_t maximumCaptureGap100ns_ = 0;
    MuxPressureLevel pressure_ = MuxPressureLevel::Healthy;
    std::chrono::steady_clock::time_point healthySince_ {};
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

void writeAvccSample(BufferedByteWriter& out, const VideoSamplePlan& sample) {
    if (!sample.packet) return;
    const auto bytes = payloadBytes(*sample.packet);
    for (const auto& nalu : sample.writableNalus) {
        if (nalu.offset + nalu.size > bytes.size()) continue;
        writeU32(out, static_cast<uint32_t>(nalu.size));
        writeBytes(out, bytes.subspan(nalu.offset, nalu.size));
    }
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
            const auto bytes = payloadBytes(packet);
            videoSourceBytes += bytes.size();
            VideoSamplePlan sample;
            sample.packet = &packet;
            sample.info.keyframe = packet.keyframe;
            auto collectNalu = [&](const NalUnit& nalu) {
                if (nalu.type == 7 && sps.empty()) {
                    sps = copyNaluPayload(bytes, nalu);
                } else if (nalu.type == 8 && pps.empty()) {
                    pps = copyNaluPayload(bytes, nalu);
                }

                if (isWritableVideoNalu(nalu)) {
                    sample.writableNalus.push_back(nalu);
                }
            };

            if (packet.h264.analyzed) {
                forEachH264Nal(packet.h264, collectNalu);
                sample.info.size = packet.h264.avccSampleSize;
            } else {
                const auto nalus = parseAnnexBNalus(bytes);
                for (const auto& nalu : nalus) collectNalu(nalu);
                sample.info.size = avccSampleSize(sample.writableNalus);
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
        // Last frame: use the packet's own duration or average of previous gaps.
        if (!videoSamples.empty()) {
            const int64_t totalSpan = videoSamples.back().packet->pts100ns - videoSamples.front().packet->pts100ns;
            const int64_t avgDuration = totalSpan > 0 && videoSamples.size() > 1
                ? totalSpan / static_cast<int64_t>(videoSamples.size() - 1)
                : 10'000'000LL / std::max(1, fps);
            videoSamples.back().info.duration = static_cast<uint32_t>(std::min<int64_t>(std::max<int64_t>(1, avgDuration), 0xFFFFFFFFLL));
        }
    } else if (videoSamples.size() == 1) {
        videoSamples[0].info.duration = static_cast<uint32_t>(10'000'000LL / std::max(1, fps));
    }
    if (videoSamples.empty()) {
        result.message = "MP4 muxing failed: the selected H.264 window contains no writable frame samples.";
        logMuxSaveTiming("total", totalStartedAt, "ok=false reason=no_writable_samples");
        return result;
    }
    const uint64_t plannedVideoDuration100ns = samplesDuration(videoSamples);
    const uint32_t targetFrameDuration100ns = static_cast<uint32_t>(10'000'000LL / std::max(1, fps));
    uint32_t maxSampleDuration100ns = 0;
    std::size_t longFrameGaps = 0;
    for (const auto& sample : videoSamples) {
        maxSampleDuration100ns = std::max(maxSampleDuration100ns, sample.info.duration);
        if (sample.info.duration > targetFrameDuration100ns * 2u) ++longFrameGaps;
    }
    logMuxSaveTiming(
        "duration_plan",
        durationStartedAt,
        "videoSamples=" + std::to_string(videoSamples.size()) +
            " plannedDuration100ns=" + std::to_string(plannedVideoDuration100ns) +
            " maxSampleDuration100ns=" + std::to_string(maxSampleDuration100ns) +
            " longFrameGaps=" + std::to_string(longFrameGaps));

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

    for (auto& track : audioTracks) finalizeAudioTimeline(track);
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

    const auto moov = makeMoov(avcConfig, videoSamples, audioTracks, width, height, fps);
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
            " lowIoPriority=" + std::string(out.lowPriorityApplied() ? "true" : "false"));

    BufferedByteWriter bufferedOut(out, Win32FileWriter::maxWriteBytes, std::move(pacing.samplePressure));
    const auto videoWriteStartedAt = SaveTimingClock::now();
    uint64_t videoWrittenBytes = 0;
    for (const auto& sample : videoSamples) {
        writeAvccSample(bufferedOut, sample);
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
            " paceYields=" + std::to_string(bufferedOut.yieldCount()) +
            " paceSleeps=" + std::to_string(videoSleeps) +
            " paceSleepMs=" + std::to_string(videoSleepMs) +
            " pressureTransitions=" + std::to_string(bufferedOut.pressureTransitions()) +
            " maxQueueAge100ns=" + std::to_string(bufferedOut.maximumQueueAge100ns()) +
            " maxNvencPending=" + std::to_string(bufferedOut.maximumNvencPending()) +
            " maxCaptureGap100ns=" + std::to_string(bufferedOut.maximumCaptureGap100ns()));

    const auto audioWriteStartedAt = SaveTimingClock::now();
    const auto audioFlushesBefore = bufferedOut.flushCount();
    const auto audioSleepsBefore = bufferedOut.sleepCount();
    const auto audioSleepMsBefore = bufferedOut.sleepMs();
    uint64_t audioWrittenBytes = 0;
    for (const auto& track : audioTracks) {
        for (const auto& sample : track.samples) {
            writeBytes(bufferedOut, samplePayload(sample));
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
            " paceSleeps=" + std::to_string(bufferedOut.sleepCount() - audioSleepsBefore) +
            " paceSleepMs=" + std::to_string(bufferedOut.sleepMs() - audioSleepMsBefore));

    const auto moovWriteStartedAt = SaveTimingClock::now();
    writeBytes(out, moov);
    logMuxSaveTiming("write_moov", moovWriteStartedAt, "bytes=" + std::to_string(moov.size()));

    const auto closeStartedAt = SaveTimingClock::now();
    const bool writeSucceeded = out.good();
    const bool closeSucceeded = out.close();
    logMuxSaveTiming("file_close", closeStartedAt);

    if (!writeSucceeded || !closeSucceeded) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        result.message = "MP4 muxing failed while writing the output file.";
        logMuxSaveTiming("total", totalStartedAt, "ok=false reason=write_failed");
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
