#pragma once
// pipe_server.h : 名前付きパイプでの制御 API サーバ(実装ガイド §5.6)
//
// 改行区切りの JSON メッセージをやり取りする最小限の JSON-RPC 的
// プロトコル(名前付きパイプ選定の理由: 追加の外部ライブラリを増やさず、
// Windows 前提の本プロジェクトと相性が良いため)。
//   リクエスト : {"id": <number>, "method": <string>, "params": {...}}
//   レスポンス : {"id": <同じ number>, "result": {...}} または
//               {"id": <同じ number>, "error": <string>}
//   通知(サーバ→クライアント、id なし): {"event": <string>, "data": {...}}
//
// 実装メモ(overlapped I/O を使う理由):
//   同期(非 overlapped)モードのパイプハンドルに対して、別スレッドから
//   ReadFile と WriteFile を同時に発行すると、Windows は同一ハンドルへの
//   同時アクセスを内部的にシリアライズすることがあり、片方が長時間
//   ブロックするコールだと(このサーバの受信ループのように、クライアント
//   からの次のメッセージを待って ReadFile がブロックし続ける)、もう
//   片方(サーバ→クライアントの通知 Notify() が別スレッドから書き込む)が
//   無期限にハングする。実際にこれで Notify() がデッドロックする不具合を
//   踏んだ。そのため受信・送信ともに FILE_FLAG_OVERLAPPED + イベントで
//   実装し、1 クライアントあたり 1 スレッドが
//   WaitForMultipleObjects で「読み込み完了」「通知キュー投入」
//   「停止要求」を待ち受ける設計にしている。
//
// スコープ(ミニマム実装の割り切り): 同時接続は 1 クライアントまで。

#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "ipc/json_value.h"

namespace ipc {

using MethodHandler = std::function<JsonValue(const JsonValue& params)>;

class PipeServer {
public:
    explicit PipeServer(std::wstring pipeName) : pipeName_(std::move(pipeName)) {
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    ~PipeServer() {
        Stop();
        CloseHandle(stopEvent_);
    }
    PipeServer(const PipeServer&) = delete;

    // Start() 前に呼ぶ想定。
    void RegisterMethod(const std::string& name, MethodHandler handler) {
        std::lock_guard<std::mutex> lock(handlersMutex_);
        handlers_[name] = std::move(handler);
    }

    void Start() {
        running_.store(true);
        thread_ = std::thread(&PipeServer::AcceptLoop, this);
    }

    void Stop() {
        if (!running_.exchange(false)) return;
        SetEvent(stopEvent_);
        if (thread_.joinable()) thread_.join();
    }

    // 接続中のクライアントへ通知(event)を送る(メータ購読等)。
    // どのスレッドから呼んでもよい(内部でキューに積むだけ)が、RT
    // スレッドからは呼ばないこと(ロック+ヒープ確保を伴うため)。
    void Notify(const JsonValue& event) {
        {
            std::lock_guard<std::mutex> lock(notifyMutex_);
            notifyQueue_.push_back(event.Dump());
        }
        std::lock_guard<std::mutex> lock(clientMutex_);
        if (clientNotifyEvent_) SetEvent(clientNotifyEvent_);
    }

private:
    // --- overlapped WriteFile を発行し、完了まで待つ(同期呼び出しのように使う) ---
    static bool WriteLineOverlapped(HANDLE pipe, HANDLE writeEvent, const std::string& text) {
        const std::string withNewline = text + "\n";
        ResetEvent(writeEvent);
        OVERLAPPED ov{};
        ov.hEvent = writeEvent;
        DWORD written = 0;
        BOOL ok = WriteFile(pipe, withNewline.data(), (DWORD)withNewline.size(), nullptr, &ov);
        if (!ok && GetLastError() != ERROR_IO_PENDING) return false;
        return GetOverlappedResult(pipe, &ov, &written, /*bWait=*/TRUE) != 0;
    }

    void AcceptLoop() {
        while (running_.load(std::memory_order_relaxed)) {
            HANDLE pipe = CreateNamedPipeW(
                pipeName_.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0, nullptr);
            if (pipe == INVALID_HANDLE_VALUE) break;

            HANDLE connectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            OVERLAPPED connectOv{};
            connectOv.hEvent = connectEvent;
            BOOL connectedNow = ConnectNamedPipe(pipe, &connectOv);
            DWORD connectErr = connectedNow ? ERROR_SUCCESS : GetLastError();

            bool connected = false;
            if (!connectedNow && connectErr == ERROR_PIPE_CONNECTED) {
                connected = true;  // クライアントが呼び出し前から待っていた
            } else if (!connectedNow && connectErr == ERROR_IO_PENDING) {
                HANDLE waitHandles[2] = { stopEvent_, connectEvent };
                DWORD wait = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
                connected = (wait == WAIT_OBJECT_0 + 1);
            } else if (connectedNow) {
                connected = true;
            }
            CloseHandle(connectEvent);

            if (!running_.load(std::memory_order_relaxed)) {
                CancelIoEx(pipe, &connectOv);
                CloseHandle(pipe);
                break;
            }
            if (!connected) {
                CancelIoEx(pipe, &connectOv);
                CloseHandle(pipe);
                continue;
            }

            ServeClient(pipe);

            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
        }
    }

    void ServeClient(HANDLE pipe) {
        HANDLE readEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE notifyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE writeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        {
            std::lock_guard<std::mutex> lock(clientMutex_);
            clientNotifyEvent_ = notifyEvent;
        }

        std::string buffer;
        char chunk[4096];
        OVERLAPPED readOv{};
        readOv.hEvent = readEvent;
        bool readPending = false;
        bool ok = true;

        while (ok) {
            if (!readPending) {
                ResetEvent(readEvent);
                ZeroMemory(&readOv, sizeof(readOv));
                readOv.hEvent = readEvent;
                BOOL immediate = ReadFile(pipe, chunk, sizeof(chunk), nullptr, &readOv);
                if (!immediate && GetLastError() != ERROR_IO_PENDING) break;  // 切断等
                readPending = true;
            }

            HANDLE waitHandles[3] = { stopEvent_, readEvent, notifyEvent };
            DWORD wait = WaitForMultipleObjects(3, waitHandles, FALSE, INFINITE);

            if (wait == WAIT_OBJECT_0) {
                break;  // サーバ停止
            } else if (wait == WAIT_OBJECT_0 + 1) {
                DWORD readBytes = 0;
                if (!GetOverlappedResult(pipe, &readOv, &readBytes, FALSE) || readBytes == 0)
                    break;
                readPending = false;
                buffer.append(chunk, readBytes);
                size_t pos;
                while ((pos = buffer.find('\n')) != std::string::npos) {
                    std::string line = buffer.substr(0, pos);
                    buffer.erase(0, pos + 1);
                    if (!line.empty()) ok = HandleLine(pipe, writeEvent, line) && ok;
                }
            } else if (wait == WAIT_OBJECT_0 + 2) {
                ResetEvent(notifyEvent);
                std::vector<std::string> toSend;
                {
                    std::lock_guard<std::mutex> lock(notifyMutex_);
                    toSend.swap(notifyQueue_);
                }
                for (const auto& msg : toSend) {
                    if (!WriteLineOverlapped(pipe, writeEvent, msg)) { ok = false; break; }
                }
            } else {
                break;  // エラー
            }
        }

        if (readPending) CancelIoEx(pipe, &readOv);

        {
            std::lock_guard<std::mutex> lock(clientMutex_);
            clientNotifyEvent_ = nullptr;
        }
        CloseHandle(readEvent);
        CloseHandle(notifyEvent);
        CloseHandle(writeEvent);
    }

    bool HandleLine(HANDLE pipe, HANDLE writeEvent, const std::string& line) {
        JsonValue request;
        try {
            request = JsonValue::Parse(line);
        } catch (const std::exception&) {
            return true;  // 不正な JSON は無視(最小実装)
        }

        const std::string method = request.At("method").AsString();
        const JsonValue id = request.At("id");

        MethodHandler handler;
        {
            std::lock_guard<std::mutex> lock(handlersMutex_);
            auto it = handlers_.find(method);
            if (it != handlers_.end()) handler = it->second;
        }

        JsonValue response = JsonValue::MakeObject();
        response["id"] = id;
        if (handler) {
            try {
                response["result"] = handler(request.At("params"));
            } catch (const std::exception& e) {
                response["error"] = std::string(e.what());
            }
        } else {
            response["error"] = std::string("unknown method: ") + method;
        }
        return WriteLineOverlapped(pipe, writeEvent, response.Dump());
    }

    std::wstring pipeName_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    HANDLE stopEvent_;

    std::mutex handlersMutex_;
    std::unordered_map<std::string, MethodHandler> handlers_;

    std::mutex notifyMutex_;
    std::vector<std::string> notifyQueue_;

    std::mutex clientMutex_;
    HANDLE clientNotifyEvent_ = nullptr;  // 現在接続中のクライアントの ServeClient が待つイベント
};

}  // namespace ipc
