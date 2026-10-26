// Creates, updates and removes the root-enumerated Win51 device, as "devcon install/update/remove" do
// (devcon itself is not redistributable). Used by setup.ps1 through "win51svc driver-install/-remove".
#pragma once

#include <windows.h>

#include <string>

namespace w51 {

constexpr wchar_t kWin51HardwareId[] = L"ROOT\\Win51";

// Creates the device node unless it exists, then installs the INF on it (also when it is not newer).
HRESULT InstallRootDevice(const std::wstring& infPath, const wchar_t* hardwareId, bool& rebootRequired);

// Removes every device node with this hardware ID. The driver package stays in the driver store.
HRESULT RemoveRootDevices(const wchar_t* hardwareId, bool& rebootRequired);

}  // namespace w51
