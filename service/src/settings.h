// Persistent settings in HKLM\SOFTWARE\Win51. The service writes them (the tray icon has no rights there),
// and so does setup.ps1.
#pragma once

#include <string>

namespace w51 {

// Output name fragment; empty = the HDMI/DisplayPort output that accepts AC3.
std::wstring LoadOutputSetting();
bool SaveOutputSetting(const std::wstring& output);

}  // namespace w51
