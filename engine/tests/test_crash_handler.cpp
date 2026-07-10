// test_crash_handler.cpp : crash_handler(engine/crash/crash_handler.h)の
// 実結合テスト。
//
// crash_handler_test_helper.exe(意図的にクラッシュする別プロセス)を
// 起動し、指定ディレクトリに .dmp ファイルが実際に生成されることを
// 確認する。子プロセスとして分離することで、ctest から見て「このテスト
// プロセス自体は正常終了する」形にしている。

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

void Fail(const std::string& what) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    std::exit(1);
}

std::wstring GetExeDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path(buf, n);
    size_t pos = path.find_last_of(L"\\/");
    return pos != std::wstring::npos ? path.substr(0, pos) : L".";
}

}  // namespace

int wmain() {
    const std::wstring exeDir = GetExeDir();
    const std::wstring helperPath = exeDir + L"\\crash_handler_test_helper.exe";
    const std::wstring dumpDir = exeDir + L"\\crash_dump_test_output\\";
    CreateDirectoryW(dumpDir.c_str(), nullptr);

    // 末尾が '\' で終わるパスをそのまま "..." で囲むと、Windows の
    // コマンドライン引数解析規則(N 個の '\' の直後の '"' は、N が偶数なら
    // N/2 個の '\' + 引用符終端、奇数なら「エスケープされた引用符文字」と
    // 解釈される)により閉じ引用符として認識されず引数が壊れる。末尾の
    // '\' を落としてから囲む(crashhandler::Install 側で末尾 '\' は
    // 自動補完されるので落としても問題ない)。
    std::wstring dumpDirArg = dumpDir;
    while (!dumpDirArg.empty() && dumpDirArg.back() == L'\\') dumpDirArg.pop_back();

    std::wstring cmdLine = L"\"" + helperPath + L"\" \"" + dumpDirArg + L"\"";
    std::vector<wchar_t> cmdLineBuf(cmdLine.begin(), cmdLine.end());
    cmdLineBuf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(helperPath.c_str(), cmdLineBuf.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &pi)) {
        Fail("failed to launch crash_handler_test_helper.exe");
    }
    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dumpDir + L"*.dmp").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        Fail("no .dmp file was created by the crashing child process");
    FindClose(h);

    std::printf("PASS: crash_handler wrote a minidump on unhandled exception\n");
    std::printf("ALL PASS: crash_handler\n");
    return 0;
}
