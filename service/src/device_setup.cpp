#include "device_setup.h"

#include <cfgmgr32.h>
#include <newdev.h>
#include <setupapi.h>

#include <functional>
#include <vector>

#include "log.h"

namespace w51 {
namespace {

HRESULT LastError() { return HRESULT_FROM_WIN32(GetLastError()); }

std::vector<std::wstring> HardwareIds(HDEVINFO set, SP_DEVINFO_DATA& dev) {
    DWORD size = 0;
    SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, nullptr, nullptr, 0, &size);
    if (size == 0) return {};
    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 2, L'\0');
    if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, nullptr, (BYTE*)buffer.data(), size,
                                           nullptr)) {
        return {};
    }
    std::vector<std::wstring> ids;
    for (const wchar_t* id = buffer.data(); *id; id += wcslen(id) + 1) ids.emplace_back(id);
    return ids;
}

// Calls fn for every root-enumerated device (present or not) that has the hardware ID.
HRESULT ForEachRootDevice(const wchar_t* hardwareId, const std::function<void(HDEVINFO, SP_DEVINFO_DATA&)>& fn) {
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"ROOT", nullptr, DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) return LastError();
    SP_DEVINFO_DATA dev = {sizeof(dev)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
        for (const auto& id : HardwareIds(set, dev)) {
            if (_wcsicmp(id.c_str(), hardwareId) == 0) {
                fn(set, dev);
                break;
            }
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return S_OK;
}

}  // namespace

HRESULT InstallRootDevice(const std::wstring& infPath, const wchar_t* hardwareId, bool& rebootRequired) {
    wchar_t inf[MAX_PATH];
    if (!GetFullPathNameW(infPath.c_str(), MAX_PATH, inf, nullptr)) return LastError();

    bool exists = false;
    HRESULT hr = ForEachRootDevice(hardwareId, [&](HDEVINFO, SP_DEVINFO_DATA&) { exists = true; });
    if (FAILED(hr)) return hr;
    if (!exists) {
        GUID classGuid;
        wchar_t className[MAX_CLASS_NAME_LEN];
        if (!SetupDiGetINFClassW(inf, &classGuid, className, MAX_CLASS_NAME_LEN, nullptr)) return LastError();
        HDEVINFO set = SetupDiCreateDeviceInfoList(&classGuid, nullptr);
        if (set == INVALID_HANDLE_VALUE) return LastError();
        SP_DEVINFO_DATA dev = {sizeof(dev)};
        const std::wstring ids = std::wstring(hardwareId) + L'\0' + L'\0';  // REG_MULTI_SZ
        const bool created =
            SetupDiCreateDeviceInfoW(set, className, &classGuid, nullptr, nullptr, DICD_GENERATE_ID, &dev) &&
            SetupDiSetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, (const BYTE*)ids.c_str(),
                                              (DWORD)(ids.size() * sizeof(wchar_t))) &&
            SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &dev);
        hr = created ? S_OK : LastError();
        SetupDiDestroyDeviceInfoList(set);
        if (FAILED(hr)) return hr;
        Log("Created device %s", Narrow(hardwareId).c_str());
    }

    BOOL reboot = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, hardwareId, inf, INSTALLFLAG_FORCE, &reboot)) return LastError();
    rebootRequired = reboot != FALSE;
    Log("Installed %s on %s", Narrow(inf).c_str(), Narrow(hardwareId).c_str());
    return S_OK;
}

HRESULT RemoveRootDevices(const wchar_t* hardwareId, bool& rebootRequired) {
    HRESULT result = S_OK;
    const HRESULT hr = ForEachRootDevice(hardwareId, [&](HDEVINFO set, SP_DEVINFO_DATA& dev) {
        BOOL reboot = FALSE;
        if (DiUninstallDevice(nullptr, set, &dev, 0, &reboot)) {
            Log("Removed device %s", Narrow(hardwareId).c_str());
        } else {
            result = LastError();
        }
        if (reboot) rebootRequired = true;
    });
    return FAILED(hr) ? hr : result;
}

}  // namespace w51
