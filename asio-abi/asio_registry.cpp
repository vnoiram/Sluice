// asio_registry.cpp : asio_registry.h の実装

#include "asio_registry.h"

#include <objbase.h>

namespace asioabi {

namespace {

// StringFromCLSID の結果("{XXXXXXXX-XXXX-...}" 形式)を std::wstring にして
// CoTaskMemFree まで済ませるヘルパー。
std::wstring ClsidToString(const CLSID& classId) {
    LPOLESTR raw = nullptr;
    if (FAILED(StringFromCLSID(classId, &raw)) || !raw) return {};
    std::wstring result(raw);
    CoTaskMemFree(raw);
    return result;
}

long WriteStringValue(HKEY key, const wchar_t* valueName, const std::wstring& value) {
    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    return RegSetValueExW(key, valueName, 0, REG_SZ,
                          reinterpret_cast<const BYTE*>(value.c_str()), bytes);
}

}  // namespace

long RegisterAsioDriver(const CLSID& classId, const std::wstring& dllPath,
                        const std::wstring& driverName, const std::wstring& description) {
    const std::wstring clsidStr = ClsidToString(classId);
    if (clsidStr.empty()) return ERROR_INVALID_PARAMETER;

    // HKEY_CLASSES_ROOT\CLSID\{...}(既定値 = 表示名)
    const std::wstring clsidKeyPath = L"CLSID\\" + clsidStr;
    HKEY clsidKey = nullptr;
    long rc = RegCreateKeyExW(HKEY_CLASSES_ROOT, clsidKeyPath.c_str(), 0, nullptr,
                              REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &clsidKey, nullptr);
    if (rc != ERROR_SUCCESS) return rc;

    rc = WriteStringValue(clsidKey, nullptr, description);
    if (rc != ERROR_SUCCESS) {
        RegCloseKey(clsidKey);
        return rc;
    }

    // \InProcServer32(既定値 = DLL パス、ThreadingModel = Apartment)
    HKEY inprocKey = nullptr;
    rc = RegCreateKeyExW(clsidKey, L"InProcServer32", 0, nullptr, REG_OPTION_NON_VOLATILE,
                         KEY_WRITE, nullptr, &inprocKey, nullptr);
    if (rc == ERROR_SUCCESS) {
        rc = WriteStringValue(inprocKey, nullptr, dllPath);
        if (rc == ERROR_SUCCESS) rc = WriteStringValue(inprocKey, L"ThreadingModel", L"Apartment");
        RegCloseKey(inprocKey);
    }
    RegCloseKey(clsidKey);
    if (rc != ERROR_SUCCESS) return rc;

    // HKEY_LOCAL_MACHINE\SOFTWARE\ASIO\<driverName>
    const std::wstring asioKeyPath = L"SOFTWARE\\ASIO\\" + driverName;
    HKEY asioKey = nullptr;
    rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, asioKeyPath.c_str(), 0, nullptr,
                         REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &asioKey, nullptr);
    if (rc != ERROR_SUCCESS) return rc;

    rc = WriteStringValue(asioKey, L"CLSID", clsidStr);
    if (rc == ERROR_SUCCESS) rc = WriteStringValue(asioKey, L"Description", description);
    RegCloseKey(asioKey);
    return rc;
}

long UnregisterAsioDriver(const CLSID& classId, const std::wstring& driverName) {
    const std::wstring clsidStr = ClsidToString(classId);
    if (clsidStr.empty()) return ERROR_INVALID_PARAMETER;

    const std::wstring clsidKeyPath = L"CLSID\\" + clsidStr;
    long rc1 = RegDeleteTreeW(HKEY_CLASSES_ROOT, clsidKeyPath.c_str());
    if (rc1 == ERROR_FILE_NOT_FOUND) rc1 = ERROR_SUCCESS;  // 既に無いのは成功扱い

    const std::wstring asioKeyPath = L"SOFTWARE\\ASIO\\" + driverName;
    long rc2 = RegDeleteTreeW(HKEY_LOCAL_MACHINE, asioKeyPath.c_str());
    if (rc2 == ERROR_FILE_NOT_FOUND) rc2 = ERROR_SUCCESS;

    return rc1 != ERROR_SUCCESS ? rc1 : rc2;
}

}  // namespace asioabi
