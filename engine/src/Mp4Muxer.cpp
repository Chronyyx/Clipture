#include "clipture/Mp4Muxer.hpp"

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
#include <sstream>
#include <utility>

namespace clipture {
namespace {

using Bytes = std::vector<uint8_t>;

struct Mp4Sample {
    std::vector<std::byte> payload;
    uint64_t fileOffset = 0;
    bool keyframe = false;
    uint32_t duration = 1;
};

struct AudioTrack {
    std::string sourceId;
    int sampleRate = 48000;
    int channels = 2;
    Bytes decoderConfig;
    std::vector<Mp4Sample> samples;
};

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
    return packet.kind == PacketKind::Video && !packet.payload.empty();
}

bool isPcmAudioPacket(const EncodedPacket& packet) {
    return packet.kind == PacketKind::Audio &&
        packet.encoderId == "PCM_S16" &&
        packet.sampleRate > 0 &&
        packet.channelCount > 0 &&
        packet.bitsPerSample == 16 &&
        !packet.payload.empty();
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

bool appendSamplePayload(IMFSample* sample, Mp4Sample& outSample) {
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

    return !outSample.payload.empty();
}

bool drainAacOutput(IMFTransform* encoder, AudioTrack& output, bool finalDrain, std::string& error) {
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

        Mp4Sample encoded;
        encoded.duration = 1024;
        if (appendSamplePayload(outSample.Get(), encoded)) {
            output.samples.push_back(std::move(encoded));
        }
    }
}

bool encodePcmTrackToAac(const AudioTrack& pcmTrack, AudioTrack& aacTrack, std::string& error) {
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

        const int64_t duration100ns = static_cast<int64_t>((10'000'000.0 * pcmSample.duration) / sampleRate);
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
    return !aacTrack.samples.empty();
}

bool startCodeAt(const std::vector<std::byte>& data, std::size_t offset, std::size_t& size) {
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

std::size_t findStartCode(const std::vector<std::byte>& data, std::size_t offset, std::size_t& size) {
    for (std::size_t i = offset; i + 3 <= data.size(); ++i) {
        if (startCodeAt(data, i, size)) return i;
    }
    return std::string::npos;
}

struct NalUnit {
    std::size_t offset = 0;
    std::size_t size = 0;
    uint8_t type = 0;
};

std::vector<NalUnit> parseAnnexBNalus(const std::vector<std::byte>& data) {
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
            nalus.push_back({ nalStart, nalEnd - nalStart, static_cast<uint8_t>(header & 0x1F) });
        }
        offset = nalEnd;
    }

    return nalus;
}

std::vector<std::byte> copyNaluPayload(const std::vector<std::byte>& data, const NalUnit& nalu) {
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

Bytes extractAvcDecoderConfig(const std::vector<EncodedPacket>& video) {
    std::vector<std::byte> sps;
    std::vector<std::byte> pps;
    for (const auto& packet : video) {
        const auto nalus = parseAnnexBNalus(packet.payload);
        for (const auto& nalu : nalus) {
            if (nalu.type == 7 && sps.empty()) {
                sps = copyNaluPayload(packet.payload, nalu);
            } else if (nalu.type == 8 && pps.empty()) {
                pps = copyNaluPayload(packet.payload, nalu);
            }
            if (!sps.empty() && !pps.empty()) return buildAvcDecoderConfig(sps, pps);
        }
    }

    return {};
}

std::vector<std::byte> annexBPacketToAvccSample(const EncodedPacket& packet) {
    const auto nalus = parseAnnexBNalus(packet.payload);
    std::vector<std::byte> sample;

    for (const auto& nalu : nalus) {
        if (nalu.type == 7 || nalu.type == 8 || nalu.type == 9) continue;
        if (nalu.size == 0 || nalu.size > 0xFFFFFFFFULL) continue;

        const auto length = static_cast<uint32_t>(nalu.size);
        sample.push_back(static_cast<std::byte>((length >> 24) & 0xFF));
        sample.push_back(static_cast<std::byte>((length >> 16) & 0xFF));
        sample.push_back(static_cast<std::byte>((length >> 8) & 0xFF));
        sample.push_back(static_cast<std::byte>(length & 0xFF));
        sample.insert(
            sample.end(),
            packet.payload.begin() + static_cast<std::ptrdiff_t>(nalu.offset),
            packet.payload.begin() + static_cast<std::ptrdiff_t>(nalu.offset + nalu.size));
    }

    return sample;
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

Bytes makeMvhd(uint32_t timescale, uint32_t duration, uint32_t nextTrackId) {
    Bytes payload;
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, timescale);
    appendU32(payload, duration);
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
    return fullBox("mvhd", 0, 0, payload);
}

Bytes makeTkhd(uint32_t duration, int width, int height) {
    Bytes payload;
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, 1);
    appendU32(payload, 0);
    appendU32(payload, duration);
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
    return fullBox("tkhd", 0, 0x000007, payload);
}

Bytes makeAudioTkhd(uint32_t trackId, uint32_t duration) {
    Bytes payload;
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, trackId);
    appendU32(payload, 0);
    appendU32(payload, duration);
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
    return fullBox("tkhd", 0, 0x000007, payload);
}

Bytes makeMdhd(uint32_t timescale, uint32_t duration) {
    Bytes payload;
    appendU32(payload, 0);
    appendU32(payload, 0);
    appendU32(payload, timescale);
    appendU32(payload, duration);
    appendU16(payload, 0x55C4);
    appendU16(payload, 0);
    return fullBox("mdhd", 0, 0, payload);
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

Bytes makeAudioStsd(const AudioTrack& track) {
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

Bytes makeStts(const std::vector<Mp4Sample>& samples) {
    Bytes payload;
    std::vector<std::pair<uint32_t, uint32_t>> entries;
    for (const auto& sample : samples) {
        const uint32_t duration = std::max<uint32_t>(1, sample.duration);
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

Bytes makeStss(const std::vector<Mp4Sample>& samples) {
    Bytes payload;
    std::vector<uint32_t> syncSamples;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if (samples[i].keyframe) syncSamples.push_back(static_cast<uint32_t>(i + 1));
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

Bytes makeStsz(const std::vector<Mp4Sample>& samples) {
    Bytes payload;
    appendU32(payload, 0);
    appendU32(payload, static_cast<uint32_t>(samples.size()));
    for (const auto& sample : samples) appendU32(payload, static_cast<uint32_t>(sample.payload.size()));
    return fullBox("stsz", 0, 0, payload);
}

Bytes makeCo64(const std::vector<Mp4Sample>& samples) {
    Bytes payload;
    appendU32(payload, static_cast<uint32_t>(samples.size()));
    for (const auto& sample : samples) appendU64(payload, sample.fileOffset);
    return fullBox("co64", 0, 0, payload);
}

uint32_t samplesDuration(const std::vector<Mp4Sample>& samples) {
    uint64_t duration = 0;
    for (const auto& sample : samples) duration += std::max<uint32_t>(1, sample.duration);
    return static_cast<uint32_t>(std::min<uint64_t>(duration, 0xFFFFFFFFULL));
}

Bytes makeVideoTrak(const Bytes& avcConfig, const std::vector<Mp4Sample>& samples, int width, int height, int /*fps*/) {
    const uint32_t timescale = 10'000'000u; // 100ns units — matches PTS-based sample durations
    const uint32_t duration = samplesDuration(samples);

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

Bytes makeAudioTrak(const AudioTrack& track, uint32_t trackId, uint32_t movieDuration) {
    const uint32_t timescale = static_cast<uint32_t>(std::max(1, track.sampleRate));
    const uint32_t duration = samplesDuration(track.samples);

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
    appendBytes(trakPayload, box("mdia", mdiaPayload));
    return box("trak", trakPayload);
}

Bytes makeMoov(const Bytes& avcConfig, const std::vector<Mp4Sample>& samples, const std::vector<AudioTrack>& audioTracks, int width, int height, int fps) {
    const uint32_t timescale = 10'000'000u; // movie timescale in 100ns units — matches video track
    const uint32_t duration = samplesDuration(samples);

    Bytes moovPayload;
    appendBytes(moovPayload, makeMvhd(timescale, duration, static_cast<uint32_t>(audioTracks.size() + 2)));
    appendBytes(moovPayload, makeVideoTrak(avcConfig, samples, width, height, fps));
    uint32_t trackId = 2;
    for (const auto& audioTrack : audioTracks) {
        appendBytes(moovPayload, makeAudioTrak(audioTrack, trackId++, duration));
    }
    return box("moov", moovPayload);
}

void writeU32(std::ofstream& out, uint32_t value) {
    const uint8_t bytes[] = {
        static_cast<uint8_t>((value >> 24) & 0xFF),
        static_cast<uint8_t>((value >> 16) & 0xFF),
        static_cast<uint8_t>((value >> 8) & 0xFF),
        static_cast<uint8_t>(value & 0xFF)
    };
    out.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

void writeU64(std::ofstream& out, uint64_t value) {
    writeU32(out, static_cast<uint32_t>((value >> 32) & 0xFFFFFFFFULL));
    writeU32(out, static_cast<uint32_t>(value & 0xFFFFFFFFULL));
}

void writeType(std::ofstream& out, const char type[4]) {
    out.write(type, 4);
}

void writeBytes(std::ofstream& out, const Bytes& bytes) {
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void writeBytes(std::ofstream& out, const std::vector<std::byte>& bytes) {
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

MuxResult muxH264ToMp4(
    const std::vector<EncodedPacket>& packets,
    const std::string& saveFolder,
    int width,
    int height,
    int fps,
    int /*bitrateMbps*/) {
    MuxResult result;

    std::vector<EncodedPacket> video;
    std::vector<AudioTrack> pcmAudioTracks;
    for (const auto& packet : packets) {
        if (isVideoPacket(packet)) {
            video.push_back(packet);
        } else if (isPcmAudioPacket(packet)) {
            auto track = std::find_if(pcmAudioTracks.begin(), pcmAudioTracks.end(), [&](const AudioTrack& candidate) {
                return candidate.sourceId == packet.sourceId &&
                    candidate.sampleRate == packet.sampleRate &&
                    candidate.channels == packet.channelCount;
            });
            if (track == pcmAudioTracks.end()) {
                pcmAudioTracks.push_back(AudioTrack {
                    packet.sourceId,
                    packet.sampleRate,
                    packet.channelCount,
                    {},
                    {}
                });
                track = std::prev(pcmAudioTracks.end());
            }

            const auto bytesPerFrame = static_cast<std::size_t>(std::max(1, packet.channelCount) * 2);
            const auto frames = static_cast<uint32_t>(std::max<std::size_t>(1, packet.payload.size() / bytesPerFrame));
            track->samples.push_back(Mp4Sample { packet.payload, 0, false, frames });
        }
    }
    if (video.empty()) {
        result.message = "No encoded H.264 packets are buffered yet.";
        return result;
    }

    const auto avcConfig = extractAvcDecoderConfig(video);
    if (avcConfig.empty()) {
        result.message = "MP4 muxing failed: the selected H.264 window has no SPS/PPS decoder header yet.";
        return result;
    }

    std::vector<Mp4Sample> samples;
    samples.reserve(video.size());
    for (const auto& packet : video) {
        auto payload = annexBPacketToAvccSample(packet);
        if (!payload.empty()) samples.push_back({ std::move(payload), 0, packet.keyframe, 0 });
    }

    // Compute per-frame durations from PTS gaps (timescale = 10,000,000 = 100ns units).
    // Previously each frame had duration=1 at timescale=fps, which assumed exactly
    // fps*seconds frames were captured. In practice WGC delivers frames at variable
    // rates and the encoder throttle drops some, so fewer frames exist, causing speedup.
    if (samples.size() >= 2) {
        // Build a parallel vector of PTS values from the matching video packets.
        // 'video' and 'samples' are not 1:1 (annexBPacketToAvccSample can return empty),
        // so we track which video packets produced a sample.
        std::vector<int64_t> samplePts;
        samplePts.reserve(video.size());
        for (const auto& packet : video) {
            auto testPayload = annexBPacketToAvccSample(packet);
            if (!testPayload.empty()) samplePts.push_back(packet.pts100ns);
        }
        for (std::size_t i = 0; i + 1 < samplePts.size(); ++i) {
            const int64_t gap = std::max<int64_t>(1, samplePts[i + 1] - samplePts[i]);
            samples[i].duration = static_cast<uint32_t>(std::min<int64_t>(gap, 0xFFFFFFFFLL));
        }
        // Last frame: use the packet's own duration or average of previous gaps.
        if (!samplePts.empty()) {
            const int64_t totalSpan = samplePts.back() - samplePts.front();
            const int64_t avgDuration = totalSpan > 0 && samplePts.size() > 1
                ? totalSpan / static_cast<int64_t>(samplePts.size() - 1)
                : 10'000'000LL / std::max(1, fps);
            samples.back().duration = static_cast<uint32_t>(std::min<int64_t>(std::max<int64_t>(1, avgDuration), 0xFFFFFFFFLL));
        }
    } else if (samples.size() == 1) {
        samples[0].duration = static_cast<uint32_t>(10'000'000LL / std::max(1, fps));
    }
    if (samples.empty()) {
        result.message = "MP4 muxing failed: the selected H.264 window contains no writable frame samples.";
        return result;
    }

    std::vector<AudioTrack> audioTracks;
    if (!pcmAudioTracks.empty()) {
        HRESULT mfHr = MFStartup(MF_VERSION);
        if (FAILED(mfHr)) {
            result.message = hresultMessage("MFStartup failed for AAC audio encoding.", mfHr);
            return result;
        }

        std::string audioError;
        for (const auto& pcmTrack : pcmAudioTracks) {
            AudioTrack aacTrack;
            if (encodePcmTrackToAac(pcmTrack, aacTrack, audioError)) {
                audioTracks.push_back(std::move(aacTrack));
            }
        }
        MFShutdown();

        if (audioTracks.empty() && !pcmAudioTracks.empty()) {
            result.message = audioError.empty()
                ? "AAC audio encoding failed for every captured audio track."
                : audioError;
            return result;
        }
    }

    samples.front().keyframe = true;

    const auto path = clipFilePath(saveFolder);
    const auto ftyp = makeFtyp();
    uint64_t mdatPayloadSize = 0;
    for (const auto& sample : samples) mdatPayloadSize += sample.payload.size();
    for (const auto& track : audioTracks) {
        for (const auto& sample : track.samples) mdatPayloadSize += sample.payload.size();
    }

    const bool largeMdat = mdatPayloadSize + 8 > 0xFFFFFFFFULL;
    const uint64_t mdatHeaderSize = largeMdat ? 16 : 8;
    uint64_t nextOffset = ftyp.size() + mdatHeaderSize;
    for (auto& sample : samples) {
        sample.fileOffset = nextOffset;
        nextOffset += sample.payload.size();
    }
    for (auto& track : audioTracks) {
        for (auto& sample : track.samples) {
            sample.fileOffset = nextOffset;
            nextOffset += sample.payload.size();
        }
    }

    const auto moov = makeMoov(avcConfig, samples, audioTracks, width, height, fps);

    std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (!out) {
        result.message = "MP4 muxing failed: could not create output file.";
        return result;
    }

    writeBytes(out, ftyp);
    if (largeMdat) {
        writeU32(out, 1);
        writeType(out, "mdat");
        writeU64(out, mdatPayloadSize + 16);
    } else {
        writeU32(out, static_cast<uint32_t>(mdatPayloadSize + 8));
        writeType(out, "mdat");
    }
    for (const auto& sample : samples) writeBytes(out, sample.payload);
    for (const auto& track : audioTracks) {
        for (const auto& sample : track.samples) writeBytes(out, sample.payload);
    }
    writeBytes(out, moov);
    out.close();

    if (!out) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        result.message = "MP4 muxing failed while writing the output file.";
        return result;
    }

    result.ok = true;
    result.filePath = narrow(path);
    result.message = "Saved MP4 clip.";
    return result;
}

}  // namespace clipture
