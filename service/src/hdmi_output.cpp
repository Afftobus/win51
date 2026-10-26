#include "hdmi_output.h"

#include <avrt.h>

#include <algorithm>

#include "iec61937.h"
#include "log.h"

namespace w51 {

HdmiOutput::~HdmiOutput() { Close(); }

void HdmiOutput::Close() {
    render_.Reset();
    client_.Reset();
    device_.Reset();
    if (event_) {
        CloseHandle(event_);
        event_ = nullptr;
    }
    bufferFrames_ = 0;
    period_ = 0;
}

HRESULT HdmiOutput::Initialize(REFERENCE_TIME period) {
    client_.Reset();
    W51_RETURN_IF_FAILED(
        device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)client_.GetAddressOf()));
    return client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period,
                               &format_.Format, nullptr);
}

HRESULT HdmiOutput::Open(IMMDevice* device, REFERENCE_TIME requestedPeriod) {
    Close();
    device_ = device;
    W51_RETURN_IF_FAILED(
        device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)client_.GetAddressOf()));

    REFERENCE_TIME defaultPeriod = 0, minPeriod = 0;
    W51_RETURN_IF_FAILED(client_->GetDevicePeriod(&defaultPeriod, &minPeriod));
    REFERENCE_TIME period = requestedPeriod > 0 ? std::max(requestedPeriod, minPeriod) : minPeriod;

    format_ = iec61937::Ac3Format();
    const char* formatName = "WAVEFORMATEXTENSIBLE/IEC61937_DOLBY_DIGITAL";
    if (client_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &format_.Format, nullptr) != S_OK) {
        WAVEFORMATEX legacy = iec61937::Ac3FormatLegacy();
        HRESULT hr = client_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &legacy, nullptr);
        if (hr != S_OK) {
            Log("HDMI output does not accept AC3 passthrough in exclusive mode: %s", HrToString(hr).c_str());
            return FAILED(hr) ? hr : AUDCLNT_E_UNSUPPORTED_FORMAT;
        }
        format_ = {};
        format_.Format = legacy;
        formatName = "WAVE_FORMAT_DOLBY_AC3_SPDIF";
    }

    HRESULT hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period,
                                     &format_.Format, nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 alignedFrames = 0;
        W51_RETURN_IF_FAILED(client_->GetBufferSize(&alignedFrames));
        period = (REFERENCE_TIME)(10000000.0 * alignedFrames / format_.Format.nSamplesPerSec + 0.5);
        Log("HDMI output: period not aligned, retrying with %u frames (%.2f ms)", alignedFrames, period / 10000.0);
        hr = Initialize(period);
    }
    if (FAILED(hr)) {
        Log("HDMI output: exclusive Initialize failed: %s", HrToString(hr).c_str());
        return hr;
    }

    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_) return HRESULT_FROM_WIN32(GetLastError());
    W51_RETURN_IF_FAILED(client_->SetEventHandle(event_));
    W51_RETURN_IF_FAILED(client_->GetBufferSize(&bufferFrames_));
    W51_RETURN_IF_FAILED(client_->GetService(IID_PPV_ARGS(render_.GetAddressOf())));
    period_ = period;

    REFERENCE_TIME latency = 0;
    client_->GetStreamLatency(&latency);
    Log("HDMI output: %s, buffer %u frames (%.2f ms), device period min %.2f / default %.2f ms, "
        "stream latency %.2f ms",
        formatName, bufferFrames_, bufferFrames_ * 1000.0 / format_.Format.nSamplesPerSec, minPeriod / 10000.0,
        defaultPeriod / 10000.0, latency / 10000.0);
    return S_OK;
}

HRESULT HdmiOutput::Run(const std::atomic<bool>& stop, const FillFn& fill) {
    BYTE* data = nullptr;
    W51_RETURN_IF_FAILED(render_->GetBuffer(bufferFrames_, &data));
    fill(data, bufferFrames_);
    W51_RETURN_IF_FAILED(render_->ReleaseBuffer(bufferFrames_, 0));

    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (!mmcss) Log("AvSetMmThreadCharacteristics failed: %lu", GetLastError());

    HRESULT hr = client_->Start();
    int timeouts = 0;
    while (SUCCEEDED(hr) && !stop.load(std::memory_order_relaxed)) {
        DWORD wait = WaitForSingleObject(event_, 1000);
        if (wait == WAIT_TIMEOUT) {
            Log("HDMI output: no buffer event for 1 s");
            if (++timeouts >= 3) hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            continue;
        }
        if (wait != WAIT_OBJECT_0) {
            hr = HRESULT_FROM_WIN32(GetLastError());
            break;
        }
        timeouts = 0;
        hr = render_->GetBuffer(bufferFrames_, &data);
        if (FAILED(hr)) break;
        fill(data, bufferFrames_);
        hr = render_->ReleaseBuffer(bufferFrames_, 0);
    }
    if (FAILED(hr)) Log("HDMI output stopped: %s", HrToString(hr).c_str());
    client_->Stop();
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    return hr;
}

}  // namespace w51
