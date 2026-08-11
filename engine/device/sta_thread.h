#pragma once
// sta_thread.h : ドライバごとの管理 STA スレッド(実装ガイド §4.1.5)
//
// ASIO ドライバは「呼び出しスレッドに単一の STA(COINIT_APARTMENTTHREADED)が
// 張り付く」という COM の作法を前提にしている。全ドライバを main.cpp が
// 起動時に 1 回だけ作る単一の STA で共有すると、あるドライバの重い呼び出し
// (control panel を開く等、内部でメッセージポンプを回すことがある)が他の
// ドライバの操作をブロックしうる。StaThread はドライバ 1 個につき専用の
// スレッド + 専用の STA を割り当てるための最小限のユーティリティ。
//
// 使い方: AsioDevice が Open() 時に 1 つ生成し、以後の全 IASIO 呼び出しを
// Invoke() 経由でこのスレッドへ委譲する(device/asio_host.cpp 参照)。
//
// 重要な注意(呼び出し側が守るべき規約): Invoke() で実行中の関数の中から
// 同じ StaThread の Invoke() を(直接・間接を問わず)再度呼んではならない。
// このスレッドは「メッセージポンプ → キューを1つ実行 → 次のメッセージへ」
// という単純なループなので、実行中のタスクの中から自分自身の完了を
// fut.get() で待つと、そのタスク自身がキューを進める役目を担っている
// ため永久にブロックする(デッドロック)。

#include <windows.h>

#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace asiohost {

class StaThread {
public:
    StaThread() {
        std::promise<HWND> ready;
        std::future<HWND> readyFuture = ready.get_future();
        thread_ = std::thread(&StaThread::Run, this, std::move(ready));
        hwnd_ = readyFuture.get();
        if (!hwnd_) {
            if (thread_.joinable()) thread_.join();
            throw std::runtime_error("StaThread: window creation failed");
        }
    }

    ~StaThread() {
        if (hwnd_) PostMessageW(hwnd_, kMsgQuit, 0, 0);
        if (thread_.joinable()) thread_.join();
    }

    StaThread(const StaThread&) = delete;
    StaThread& operator=(const StaThread&) = delete;

    // ドライバの init() に渡すウィンドウハンドル(罠3: 専用の隠しウィンドウが
    // 要るドライバもある、asio_host.h の初学者向けコメント参照)。
    HWND Hwnd() const { return hwnd_; }

    // fn をこのスレッド上で実行し、完了まで呼び出し元をブロックして結果
    // (または fn が投げた例外)を返す。
    template <class F>
    auto Invoke(F&& fn) -> std::invoke_result_t<F> {
        using R = std::invoke_result_t<F>;
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(fn));
        std::future<R> fut = task->get_future();
        {
            std::lock_guard<std::mutex> lock(mu_);
            queue_.push_back([task] { (*task)(); });
        }
        PostMessageW(hwnd_, kMsgInvoke, 0, 0);
        return fut.get();
    }

private:
    static constexpr UINT kMsgInvoke = WM_APP + 1;
    static constexpr UINT kMsgQuit   = WM_APP + 2;

    void Run(std::promise<HWND> ready) {
        const bool comInit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));

        static const wchar_t kClassName[] = L"SluiceAsioStaThreadWindow";
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClassName;
        // 2 回目以降の呼び出しは ERROR_CLASS_ALREADY_EXISTS で失敗するが、
        // それは「既に登録済み」という意味なので無視してよい
        // (RegisterClassW 自体は複数スレッドから並行に呼んでも安全)。
        RegisterClassW(&wc);

        HWND hwnd = CreateWindowExW(0, kClassName, L"SluiceAsioSta", 0, 0, 0, 0, 0,
                                    nullptr, nullptr, wc.hInstance, nullptr);
        ready.set_value(hwnd);
        if (!hwnd) {
            if (comInit) CoUninitialize();
            return;
        }

        MSG msg;
        bool quit = false;
        while (!quit && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.hwnd == hwnd && msg.message == kMsgInvoke) {
                DrainQueue();
            } else if (msg.hwnd == hwnd && msg.message == kMsgQuit) {
                quit = true;
            } else {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        DrainQueue();  // 終了直前にキューへ残っていれば実行しておく(通常は空)
        DestroyWindow(hwnd);
        if (comInit) CoUninitialize();
    }

    void DrainQueue() {
        for (;;) {
            std::function<void()> task;
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (queue_.empty()) return;
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            task();
        }
    }

    std::thread thread_;
    HWND hwnd_ = nullptr;
    std::mutex mu_;
    std::deque<std::function<void()>> queue_;
};

} // namespace asiohost
