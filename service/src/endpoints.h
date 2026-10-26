// Render endpoint enumeration and lookup.
#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <string>
#include <vector>

namespace w51 {

using Microsoft::WRL::ComPtr;

struct EndpointInfo {
    std::wstring id;
    std::wstring name;         // e.g. "LG TV (NVIDIA High Definition Audio)"
    std::wstring adapterName;  // e.g. "NVIDIA High Definition Audio"
    DWORD state = 0;           // DEVICE_STATE_*
    ComPtr<IMMDevice> device;
};

std::vector<EndpointInfo> ListRenderEndpoints(DWORD stateMask);

// Case-insensitive substring match on the endpoint name or the adapter name, active endpoints only.
HRESULT FindRenderEndpoint(const std::wstring& nameSubstring, EndpointInfo& out);

// Active HDMI/DisplayPort endpoint that accepts AC3 over IEC 61937 in exclusive mode (the default device
// wins a tie). S/PDIF and the Win51 endpoint itself are never picked.
HRESULT FindAc3PassthroughEndpoint(EndpointInfo& out);

// True if the device accepts AC3 over IEC 61937 (2ch 16-bit 48 kHz) in exclusive mode.
bool SupportsExclusiveAc3(IMMDevice* device);

// PKEY_AudioEndpoint_FormFactor (EndpointFormFactor), UnknownFormFactor if it is not set.
UINT EndpointFormFactorOf(IMMDevice* device);

bool ContainsNoCase(const std::wstring& haystack, const std::wstring& needle);

// Name fragment that identifies the Win51 virtual endpoint (INF device description).
constexpr wchar_t kWin51EndpointName[] = L"Win51";

}  // namespace w51
