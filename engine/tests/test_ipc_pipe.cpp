// test_ipc_pipe.cpp : PipeServer(engine/ipc/pipe_server.h)の実結合テスト。
//
// 名前付きパイプはオーディオデバイス/サービス不要の基本 OS 機能なので、
// Windows Server Core ベースのビルドコンテナでも実際に接続・送受信まで
// 検証できる(test_wasapi_compile.cpp と違い、ここでは実際に呼び出す)。
//
// 同一プロセス内でサーバを起動し、クライアントとして同じ名前付き
// パイプへ CreateFileW で接続してリクエスト/レスポンス、通知(event)の
// 送受信を確認する。

#include "ipc/pipe_server.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

namespace {

void Fail(const std::string& what) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    std::exit(1);
}

// クライアント側の同期ヘルパ。改行までを 1 メッセージとして読む。
class TestClient {
public:
    explicit TestClient(const std::wstring& pipeName) {
        // サーバがまだ ConnectNamedPipe で待機開始していない可能性が
        // あるため、少数回リトライする。
        for (int i = 0; i < 50; ++i) {
            pipe_ = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, 0, nullptr);
            if (pipe_ != INVALID_HANDLE_VALUE) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    ~TestClient() {
        if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
    }
    bool Connected() const { return pipe_ != INVALID_HANDLE_VALUE; }

    void SendLine(const std::string& line) {
        const std::string withNewline = line + "\n";
        DWORD written = 0;
        WriteFile(pipe_, withNewline.data(), (DWORD)withNewline.size(), &written, nullptr);
    }

    // 改行区切りで 1 メッセージ読む(タイムアウトなし。テストの
    // ウォッチドッグ(CMake の TIMEOUT プロパティ)に任せる)。
    std::string ReadLine() {
        while (true) {
            size_t pos = buffer_.find('\n');
            if (pos != std::string::npos) {
                std::string line = buffer_.substr(0, pos);
                buffer_.erase(0, pos + 1);
                return line;
            }
            char chunk[4096];
            DWORD read = 0;
            if (!ReadFile(pipe_, chunk, sizeof(chunk), &read, nullptr) || read == 0)
                return "";
            buffer_.append(chunk, read);
        }
    }

private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::string buffer_;
};

}  // namespace

int main() {
    // 同一マシンで複数回テストを回しても衝突しないよう PID をパイプ名に混ぜる。
    const std::wstring pipeName =
        L"\\\\.\\pipe\\sluice-engine-test-" + std::to_wstring(GetCurrentProcessId());

    ipc::PipeServer server(pipeName);

    server.RegisterMethod("echo", [](const JsonValue& params) -> JsonValue {
        JsonValue result = JsonValue::MakeObject();
        result["echoed"] = params.At("value").AsString();
        return result;
    });
    server.RegisterMethod("set_param", [](const JsonValue& params) -> JsonValue {
        JsonValue result = JsonValue::MakeObject();
        result["stripIndex"] = params.At("stripIndex").AsInt();
        result["gainDb"] = params.At("gainDb").AsNumber();
        result["applied"] = true;
        return result;
    });
    server.RegisterMethod("boom", [](const JsonValue&) -> JsonValue {
        throw std::runtime_error("intentional test failure");
    });

    server.Start();

    auto client = std::make_unique<TestClient>(pipeName);
    if (!client->Connected()) Fail("client failed to connect to pipe server");

    // --- リクエスト/レスポンス: echo ---
    client->SendLine(R"({"id":1,"method":"echo","params":{"value":"hello"}})");
    JsonValue resp1 = JsonValue::Parse(client->ReadLine());
    if (resp1.At("id").AsInt() != 1) Fail("echo: id mismatch");
    if (resp1.At("result").At("echoed").AsString() != "hello") Fail("echo: value mismatch");

    // --- リクエスト/レスポンス: set_param ---
    client->SendLine(R"({"id":2,"method":"set_param","params":{"stripIndex":3,"gainDb":-6}})");
    JsonValue resp2 = JsonValue::Parse(client->ReadLine());
    if (resp2.At("id").AsInt() != 2) Fail("set_param: id mismatch");
    if (resp2.At("result").At("stripIndex").AsInt() != 3)
        Fail("set_param: stripIndex mismatch");
    if (!resp2.At("result").At("applied").AsBool()) Fail("set_param: applied should be true");

    // --- 未知のメソッド ---
    client->SendLine(R"({"id":3,"method":"nope","params":{}})");
    JsonValue resp3 = JsonValue::Parse(client->ReadLine());
    if (resp3.At("error").IsNull()) Fail("unknown method: expected error field");

    // --- ハンドラが例外を投げた場合もエラーとして返る(サーバは落ちない) ---
    client->SendLine(R"({"id":4,"method":"boom","params":{}})");
    JsonValue resp4 = JsonValue::Parse(client->ReadLine());
    if (resp4.At("error").IsNull()) Fail("boom: expected error field");

    // --- サーバ→クライアント通知(メータ購読を模した push) ---
    // Notify() は別スレッド(このテストのメインスレッド)から呼ぶ。
    // ServeClient 側は受信待ちの ReadFile と同時に通知イベントも
    // WaitForMultipleObjects で待っているため、受信待ち中でも通知が
    // 届く(overlapped I/O 化前は、この組み合わせでデッドロックする
    // 不具合があった)。
    JsonValue event = JsonValue::MakeObject();
    event["event"] = std::string("meters");
    JsonValue data = JsonValue::MakeArray();
    data.Push(0.5);
    data.Push(0.25);
    event["data"] = data;
    server.Notify(event);

    JsonValue received = JsonValue::Parse(client->ReadLine());
    if (received.At("event").AsString() != "meters") Fail("notify: event field mismatch");
    if (received.At("data").Items().size() != 2) Fail("notify: data array size mismatch");

    // --- 一度切断して再接続しても新しいセッションとして応答できる ---
    client.reset();
    client = std::make_unique<TestClient>(pipeName);
    if (!client->Connected()) Fail("reconnect: client failed to reconnect");
    client->SendLine(R"({"id":5,"method":"echo","params":{"value":"again"}})");
    JsonValue resp5 = JsonValue::Parse(client->ReadLine());
    if (resp5.At("result").At("echoed").AsString() != "again") Fail("reconnect: echo mismatch");

    std::printf(
        "PASS: ipc pipe (echo/set_param/unknown-method/exception/notify/reconnect all OK)\n");

    // Stop() はクライアントが接続されたままでも安全に返る(内部で
    // stopEvent_ を ServeClient/AcceptLoop 双方の WaitForMultipleObjects
    // に含めているため)。ここではあえて接続したままにして、その挙動も
    // 検証する。
    server.Stop();

    std::printf("ALL PASS: ipc_pipe\n");
    return 0;
}
