#include "clipture/AacEncoderSession.hpp"

#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <sstream>

namespace clipture {
namespace {

std::string hresultMessage(const char* prefix, HRESULT hr) {
    std::ostringstream out;
    out << prefix << " HRESULT 0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr) << ".";
    return out.str();
}

class ThreadMediaFoundationRuntime {
public:
    bool ensure(std::string& error) {
        if (initialized_) {
            if (SUCCEEDED(startupResult_)) return true;
            error = hresultMessage("Media Foundation runtime initialization failed.", startupResult_);
            return false;
        }
        initialized_ = true;

        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(comResult)) {
            comInitialized_ = true;
        } else if (comResult != RPC_E_CHANGED_MODE) {
            startupResult_ = comResult;
            error = hresultMessage("COM initialization failed for AAC encoding.", comResult);
            return false;
        }

        startupResult_ = MFStartup(MF_VERSION);
        if (FAILED(startupResult_)) {
            error = hresultMessage("MFStartup failed for AAC encoding.", startupResult_);
            return false;
        }
        mfStarted_ = true;
        return true;
    }

    ~ThreadMediaFoundationRuntime() {
        if (mfStarted_) MFShutdown();
        if (comInitialized_) CoUninitialize();
    }

private:
    bool initialized_ = false;
    bool mfStarted_ = false;
    bool comInitialized_ = false;
    HRESULT startupResult_ = S_OK;
};

ThreadMediaFoundationRuntime& threadMediaFoundationRuntime() {
    thread_local ThreadMediaFoundationRuntime runtime;
    return runtime;
}

bool copySamplePayload(IMFSample* sample, AacEncodedFrame& frame) {
    if (!sample) return false;
    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return false;

    BYTE* data = nullptr;
    DWORD maxLength = 0;
    DWORD currentLength = 0;
    if (FAILED(buffer->Lock(&data, &maxLength, &currentLength))) return false;
    frame.payload.resize(currentLength);
    if (currentLength > 0 && data) std::memcpy(frame.payload.data(), data, currentLength);
    buffer->Unlock();
    return !frame.payload.empty();
}

}  // namespace

struct AacEncoderSession::Impl {
    Microsoft::WRL::ComPtr<IMFTransform> encoder;
    Microsoft::WRL::ComPtr<IMFSample> reusableOutputSample;
    Microsoft::WRL::ComPtr<IMFMediaBuffer> reusableOutputBuffer;
    int sampleRate = 0;
    int channels = 0;
    DWORD outputBufferSize = 0;
    bool outputProvidesSamples = false;
    bool streamStarted = false;
    int64_t epochStartPts100ns = 0;
    int64_t nextOutputPts100ns = 0;
    bool firstOutput = true;

    bool drain(std::vector<AacEncodedFrame>& output, std::string& error) {
        while (true) {
            if (!outputProvidesSamples && reusableOutputBuffer) {
                const HRESULT clearHr = reusableOutputBuffer->SetCurrentLength(0);
                if (FAILED(clearHr)) {
                    error = hresultMessage("AAC output buffer reset failed.", clearHr);
                    return false;
                }
            }

            MFT_OUTPUT_DATA_BUFFER outputData {};
            outputData.dwStreamID = 0;
            outputData.pSample = outputProvidesSamples ? nullptr : reusableOutputSample.Get();
            DWORD processStatus = 0;
            const HRESULT hr = encoder->ProcessOutput(0, 1, &outputData, &processStatus);
            if (outputData.pEvents) outputData.pEvents->Release();
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
            if (FAILED(hr)) {
                if (outputProvidesSamples && outputData.pSample) outputData.pSample->Release();
                error = hresultMessage("AAC encoder output failed.", hr);
                return false;
            }

            IMFSample* produced = outputData.pSample;
            AacEncodedFrame frame;
            LONGLONG relativeTime = 0;
            if (produced && SUCCEEDED(produced->GetSampleTime(&relativeTime))) {
                frame.pts100ns = epochStartPts100ns + relativeTime;
            } else {
                frame.pts100ns = nextOutputPts100ns;
            }
            if (firstOutput) {
                const int64_t priming100ns = std::max<int64_t>(0, epochStartPts100ns - frame.pts100ns);
                frame.primingFrames = static_cast<int32_t>(std::min<int64_t>(
                    (priming100ns * sampleRate + 5'000'000LL) / 10'000'000LL,
                    std::numeric_limits<int32_t>::max()));
                firstOutput = false;
            }
            LONGLONG duration100ns = 0;
            if (produced && SUCCEEDED(produced->GetSampleDuration(&duration100ns)) && duration100ns > 0) {
                frame.durationFrames = static_cast<uint32_t>(std::max<int64_t>(
                    1,
                    (duration100ns * sampleRate + 5'000'000LL) / 10'000'000LL));
            }
            if (copySamplePayload(produced, frame)) {
                nextOutputPts100ns = frame.pts100ns +
                    (static_cast<int64_t>(frame.durationFrames) * 10'000'000LL) / sampleRate;
                output.push_back(std::move(frame));
            }
            if (outputProvidesSamples && produced) produced->Release();
        }
    }
};

AacEncoderSession::AacEncoderSession()
    : impl_(std::make_unique<Impl>()) {}

AacEncoderSession::~AacEncoderSession() {
    reset();
}

bool AacEncoderSession::start(int sampleRate, int channels, std::string& error) {
    reset();
    impl_ = std::make_unique<Impl>();
    impl_->sampleRate = std::max(8000, sampleRate);
    impl_->channels = std::clamp(channels, 1, 2);

    if (!threadMediaFoundationRuntime().ensure(error)) return false;

    MFT_REGISTER_TYPE_INFO outputInfo { MFMediaType_Audio, MFAudioFormat_AAC };
    IMFActivate** activates = nullptr;
    UINT32 activateCount = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER, MFT_ENUM_FLAG_ALL, nullptr, &outputInfo, &activates, &activateCount);
    if (FAILED(hr) || activateCount == 0 || !activates) {
        error = hresultMessage("No Media Foundation AAC encoder found.", FAILED(hr) ? hr : MF_E_TOPO_CODEC_NOT_FOUND);
        reset();
        return false;
    }

    hr = activates[0]->ActivateObject(IID_PPV_ARGS(&impl_->encoder));
    for (UINT32 i = 0; i < activateCount; ++i) activates[i]->Release();
    CoTaskMemFree(activates);
    if (FAILED(hr)) {
        error = hresultMessage("AAC encoder activation failed.", hr);
        reset();
        return false;
    }

    Microsoft::WRL::ComPtr<IMFMediaType> inputType;
    if (FAILED(MFCreateMediaType(&inputType))) {
        error = "AAC input media type allocation failed.";
        reset();
        return false;
    }
    inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    inputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    inputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, static_cast<UINT32>(impl_->channels));
    inputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(impl_->sampleRate));
    inputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    inputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, static_cast<UINT32>(impl_->channels * 2));
    inputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, static_cast<UINT32>(impl_->sampleRate * impl_->channels * 2));
    hr = impl_->encoder->SetInputType(0, inputType.Get(), 0);
    if (FAILED(hr)) {
        error = hresultMessage("AAC encoder input type failed.", hr);
        reset();
        return false;
    }

    const int bitrate = impl_->channels == 1 ? 96000 : 160000;
    Microsoft::WRL::ComPtr<IMFMediaType> selectedOutputType;
    for (DWORD index = 0;; ++index) {
        Microsoft::WRL::ComPtr<IMFMediaType> candidate;
        hr = impl_->encoder->GetOutputAvailableType(0, index, &candidate);
        if (hr == MF_E_NO_MORE_TYPES) break;
        if (FAILED(hr)) continue;
        GUID subtype {};
        if (FAILED(candidate->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFAudioFormat_AAC) continue;
        candidate->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, static_cast<UINT32>(impl_->channels));
        candidate->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(impl_->sampleRate));
        candidate->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, static_cast<UINT32>(bitrate / 8));
        candidate->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        candidate->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        candidate->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
        if (SUCCEEDED(impl_->encoder->SetOutputType(0, candidate.Get(), 0))) {
            selectedOutputType = candidate;
            break;
        }
    }
    if (!selectedOutputType) {
        error = "AAC encoder has no compatible LC output type.";
        reset();
        return false;
    }

    MFT_OUTPUT_STREAM_INFO streamInfo {};
    hr = impl_->encoder->GetOutputStreamInfo(0, &streamInfo);
    if (FAILED(hr)) {
        error = hresultMessage("AAC encoder output stream info failed.", hr);
        reset();
        return false;
    }
    impl_->outputProvidesSamples = (streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
    impl_->outputBufferSize = std::max<DWORD>(streamInfo.cbSize, 64 * 1024);
    if (!impl_->outputProvidesSamples) {
        hr = MFCreateSample(&impl_->reusableOutputSample);
        if (SUCCEEDED(hr)) hr = MFCreateMemoryBuffer(impl_->outputBufferSize, &impl_->reusableOutputBuffer);
        if (SUCCEEDED(hr)) hr = impl_->reusableOutputSample->AddBuffer(impl_->reusableOutputBuffer.Get());
        if (FAILED(hr)) {
            error = hresultMessage("AAC reusable output allocation failed.", hr);
            reset();
            return false;
        }
    }

    impl_->encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    impl_->encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    impl_->streamStarted = true;
    return true;
}

bool AacEncoderSession::encode(
    std::span<const std::byte> pcm,
    int64_t pts100ns,
    uint32_t frameCount,
    std::vector<AacEncodedFrame>& output,
    std::string& error) {
    if (!active() || pcm.empty() || frameCount == 0) return false;
    if (impl_->epochStartPts100ns == 0) {
        impl_->epochStartPts100ns = pts100ns;
        impl_->nextOutputPts100ns = pts100ns;
    }

    Microsoft::WRL::ComPtr<IMFSample> sample;
    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) hr = MFCreateMemoryBuffer(static_cast<DWORD>(pcm.size()), &buffer);
    if (FAILED(hr)) {
        error = hresultMessage("AAC input batch allocation failed.", hr);
        return false;
    }

    BYTE* destination = nullptr;
    DWORD maxLength = 0;
    DWORD currentLength = 0;
    hr = buffer->Lock(&destination, &maxLength, &currentLength);
    if (FAILED(hr)) {
        error = hresultMessage("AAC input batch lock failed.", hr);
        return false;
    }
    std::memcpy(destination, pcm.data(), pcm.size());
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(pcm.size()));
    sample->AddBuffer(buffer.Get());
    const int64_t duration100ns = (static_cast<int64_t>(frameCount) * 10'000'000LL) / impl_->sampleRate;
    sample->SetSampleTime(pts100ns - impl_->epochStartPts100ns);
    sample->SetSampleDuration(duration100ns);

    while (true) {
        hr = impl_->encoder->ProcessInput(0, sample.Get(), 0);
        if (hr != MF_E_NOTACCEPTING) break;
        if (!impl_->drain(output, error)) return false;
    }
    if (FAILED(hr)) {
        error = hresultMessage("AAC encoder input failed.", hr);
        return false;
    }
    return impl_->drain(output, error);
}

bool AacEncoderSession::finish(std::vector<AacEncodedFrame>& output, std::string& error) {
    if (!active()) return true;
    impl_->encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    impl_->encoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    const bool ok = impl_->drain(output, error);
    impl_->encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    impl_->streamStarted = false;
    return ok;
}

void AacEncoderSession::reset() {
    if (!impl_) return;
    impl_->reusableOutputBuffer.Reset();
    impl_->reusableOutputSample.Reset();
    impl_->encoder.Reset();
    impl_ = std::make_unique<Impl>();
}

bool AacEncoderSession::active() const {
    return impl_ && impl_->encoder && impl_->streamStarted;
}

int AacEncoderSession::sampleRate() const {
    return impl_ ? impl_->sampleRate : 0;
}

int AacEncoderSession::channels() const {
    return impl_ ? impl_->channels : 0;
}

}  // namespace clipture
