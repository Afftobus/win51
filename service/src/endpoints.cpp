#include "endpoints.h"

#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include "iec61937.h"

namespace w51 {
namespace {

std::wstring GetStringProperty(IPropertyStore* props, const PROPERTYKEY& key) {
    PROPVARIANT v;
    PropVariantInit(&v);
    std::wstring result;
    if (SUCCEEDED(props->GetValue(key, &v)) && v.vt == VT_LPWSTR && v.pwszVal) result = v.pwszVal;
    PropVariantClear(&v);
    return result;
}

}  // namespace

bool SupportsExclusiveAc3(IMMDevice* device) {
    ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)client.GetAddressOf()))) {
        return false;
    }
    WAVEFORMATEXTENSIBLE wfx = iec61937::Ac3Format();
    if (client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &wfx.Format, nullptr) == S_OK) return true;
    WAVEFORMATEX legacy = iec61937::Ac3FormatLegacy();
    return client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &legacy, nullptr) == S_OK;
}

bool ContainsNoCase(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) return true;
    return FindNLSStringEx(LOCALE_NAME_INVARIANT, FIND_FROMSTART | NORM_IGNORECASE, haystack.c_str(),
                           (int)haystack.size(), needle.c_str(), (int)needle.size(), nullptr, nullptr, nullptr,
                           0) >= 0;
}

std::vector<EndpointInfo> ListRenderEndpoints(DWORD stateMask) {
    std::vector<EndpointInfo> result;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(enumerator.GetAddressOf())))) {
        return result;
    }
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, stateMask, collection.GetAddressOf()))) return result;

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        EndpointInfo info;
        if (FAILED(collection->Item(i, info.device.GetAddressOf()))) continue;
        LPWSTR id = nullptr;
        if (SUCCEEDED(info.device->GetId(&id))) {
            info.id = id;
            CoTaskMemFree(id);
        }
        info.device->GetState(&info.state);
        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(info.device->OpenPropertyStore(STGM_READ, props.GetAddressOf()))) {
            info.name = GetStringProperty(props.Get(), PKEY_Device_FriendlyName);
            info.adapterName = GetStringProperty(props.Get(), PKEY_DeviceInterface_FriendlyName);
        }
        result.push_back(std::move(info));
    }
    return result;
}

HRESULT FindRenderEndpoint(const std::wstring& nameSubstring, EndpointInfo& out) {
    for (auto& ep : ListRenderEndpoints(DEVICE_STATE_ACTIVE)) {
        if (ContainsNoCase(ep.name, nameSubstring) || ContainsNoCase(ep.adapterName, nameSubstring)) {
            out = std::move(ep);
            return S_OK;
        }
    }
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

HRESULT FindAc3PassthroughEndpoint(EndpointInfo& out) {
    // PKEY_AudioEndpoint_FormFactor; HDMI and DisplayPort outputs report DigitalAudioDisplayDevice.
    static const PROPERTYKEY kFormFactor = {{0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 0};

    std::wstring defaultId;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   IID_PPV_ARGS(enumerator.GetAddressOf())))) {
        ComPtr<IMMDevice> def;
        LPWSTR id = nullptr;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, def.GetAddressOf())) &&
            SUCCEEDED(def->GetId(&id))) {
            defaultId = id;
            CoTaskMemFree(id);
        }
    }

    // HDMI/DisplayPort only. A motherboard S/PDIF jack accepts AC3 as well; taking it while the TV is off
    // would leave the stream there after the TV comes back on.
    int bestScore = -1;
    for (auto& ep : ListRenderEndpoints(DEVICE_STATE_ACTIVE)) {
        if (ContainsNoCase(ep.name, kWin51EndpointName)) continue;
        bool display = false;
        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(ep.device->OpenPropertyStore(STGM_READ, props.GetAddressOf()))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            display = SUCCEEDED(props->GetValue(kFormFactor, &v)) && v.vt == VT_UI4 &&
                      v.ulVal == DigitalAudioDisplayDevice;
            PropVariantClear(&v);
        }
        if (!display || !SupportsExclusiveAc3(ep.device.Get())) continue;
        const int score = ep.id == defaultId ? 1 : 0;
        if (score > bestScore) {
            bestScore = score;
            out = std::move(ep);
        }
    }
    return bestScore >= 0 ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

}  // namespace w51
