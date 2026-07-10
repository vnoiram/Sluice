// crash_handler.cpp : クラッシュダンプ収集の実装
#include "crash/crash_handler.h"

#include <windows.h>
#include <dbghelp.h>

#include <cwchar>

#pragma comment(lib, "dbghelp.lib")

namespace crashhandler {

namespace {

std::wstring g_dumpDir;

std::wstring DefaultDumpDir() {
    wchar_t buf[MAX_PATH]{};
    DWORD n = GetTempPathW(MAX_PATH, buf);
    return n > 0 ? std::wstring(buf, n) : L".\\";
}

std::wstring MakeDumpPath() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[96];
    swprintf_s(name, L"sluice-engine-%04d%02d%02d-%02d%02d%02d-%lu.dmp", st.wYear,
              st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
              GetCurrentProcessId());
    return g_dumpDir + name;
}

// 「事実上必ず致命的」と見なす例外コードのみダンプ対象にする。C++ 例外
// (0xE06D7363)やブレークポイント等、正常系で発生しうるものは無視する
// (VEH は __try/__except で後から回復される例外にも先んじて呼ばれるため、
// フィルタしないとダンプが乱発する)。
bool IsFatalExceptionCode(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
        return true;
    default:
        return false;
    }
}

void WriteDump(EXCEPTION_POINTERS* info) {
    const std::wstring path = MakeDumpPath();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = info;
    mei.ClientPointers = FALSE;
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal,
                      info ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(file);
}

// Vectored Exception Handler(実装ガイド §5.8 のクラッシュダンプ収集)。
// SetUnhandledExceptionFilter よりも早い段階(SEH フレームベースの
// __try/__except より前)で呼ばれるため、より確実にダンプを取得できる。
// EXCEPTION_CONTINUE_SEARCH を返し、例外そのものは消費しない(あとで
// __try/__except に回復されるかどうかの通常の解決に委ねる。ダンプを
// 書くのはあくまで「観測」であって「処理」ではない)。
LONG CALLBACK VectoredHandler(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (!IsFatalExceptionCode(info->ExceptionRecord->ExceptionCode))
        return EXCEPTION_CONTINUE_SEARCH;
    WriteDump(info);
    return EXCEPTION_CONTINUE_SEARCH;
}

// 最後の砦として SetUnhandledExceptionFilter も残しておく(VEH が何らかの
// 理由で効かない環境向けのフォールバック。害はない)。
LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* info) {
    WriteDump(info);
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

void Install(const std::wstring& dumpDirectory) {
    g_dumpDir = dumpDirectory.empty() ? DefaultDumpDir() : dumpDirectory;
    if (!g_dumpDir.empty() && g_dumpDir.back() != L'\\') g_dumpDir += L'\\';
    CreateDirectoryW(g_dumpDir.c_str(), nullptr);  // 既存なら何もしない

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    AddVectoredExceptionHandler(/*First=*/1, VectoredHandler);
    SetUnhandledExceptionFilter(UnhandledFilter);
}

}  // namespace crashhandler
