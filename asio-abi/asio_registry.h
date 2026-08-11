#pragma once
// asio_registry.h : ASIO ドライバの COM 登録/解除に必要なレジストリ操作。
//
// vasio.dll が以前 ASIO SDK 付属の common/register.cpp
// (RegisterAsioDriver/UnregisterAsioDriver)からそのままリンクしていたロジック
// の独自(SDK 非依存)代替。ASIO 固有のロジックは無く、素の Win32 レジストリ
// API 呼び出しのみ:
//   HKEY_CLASSES_ROOT\CLSID\{...}                   (既定値 = 表示名)
//     \InProcServer32                                (既定値 = DLL パス、
//                                                      ThreadingModel = Apartment)
//   HKEY_LOCAL_MACHINE\SOFTWARE\ASIO\<driverName>
//     CLSID       = "{...}" 文字列形式
//     Description = 表示名
//
// SDK 版(common/register.cpp)は Windows 9x 時代の名残で ANSI(char*)専用の
// Win32 API を使っていたため、vasio/CMakeLists.txt は UNICODE/_UNICODE の
// 定義を意図的に外していた。この独自実装はワイド文字版 API
// (RegCreateKeyExW 等)で統一しているため、その制約は無い(プロジェクトの
// 他ターゲットと同じ UNICODE ビルドにできる)。

// WIN32_LEAN_AND_MEAN 付きで <windows.h> を先にインクルードすると
// <objbase.h> が自動では入らない罠がある(asio_abi.h と同じ理由)。
// レジストリ API(RegCreateKeyExW 等)は <windows.h> 側(winreg.h)にある。
#include <windows.h>
#include <objbase.h>

#include <string>

namespace asioabi {

// 戻り値は Win32 のエラーコード(ERROR_SUCCESS(0)で成功)。
//   classId    : ドライバの CLSID。
//   dllPath    : InProcServer32 の既定値に書く DLL のフルパス。
//   driverName : HKLM\SOFTWARE\ASIO 配下のサブキー名として使う表示名
//                (regsvr32 で読み込まれる .dll のファイル名とは無関係)。
//   description: HKCR\CLSID\{...} の既定値と、ASIO 側の Description 値の
//                両方に使う。
long RegisterAsioDriver(const CLSID& classId, const std::wstring& dllPath,
                        const std::wstring& driverName, const std::wstring& description);

long UnregisterAsioDriver(const CLSID& classId, const std::wstring& driverName);

}  // namespace asioabi
