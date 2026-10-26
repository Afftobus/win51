#include "settings.h"

#include <windows.h>

namespace w51 {
namespace {

constexpr wchar_t kKey[] = L"SOFTWARE\\Win51";
constexpr wchar_t kOutputValue[] = L"Output";

}  // namespace

std::wstring LoadOutputSetting() {
    wchar_t value[512];
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kKey, kOutputValue, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) {
        return {};
    }
    return value;
}

bool SaveOutputSetting(const std::wstring& output) {
    return RegSetKeyValueW(HKEY_LOCAL_MACHINE, kKey, kOutputValue, REG_SZ, output.c_str(),
                           (DWORD)((output.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

}  // namespace w51
