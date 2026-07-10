// crash_handler_test_helper.cpp : test_crash_handler.cpp から子プロセスと
// して起動されるヘルパ。crashhandler::Install() を呼んでから意図的に
// クラッシュし、ダンプが実際に書き出されることを確認できるようにする。

#include "crash/crash_handler.h"

#include <windows.h>

int wmain(int argc, wchar_t** argv) {
    std::wstring dumpDir = (argc > 1) ? argv[1] : L"";
    crashhandler::Install(dumpDir);

    // 意図的なクラッシュ(nullptr 参照によるアクセス違反)。
    volatile int* p = nullptr;
    *p = 42;
    return 0;  // 到達しない
}
