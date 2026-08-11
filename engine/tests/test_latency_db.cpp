// test_latency_db.cpp : ipc/latency_db.h のオフライン回帰テスト(gap 8)。
// tools/latencybench --json の出力形を模した JSON をパースし、デバイス名の
// 部分一致ルックアップが期待どおり動くことを確認する。Windows API に
// 依存しないため test_device_report_json.cpp と同じく常にビルド・実行できる。

#include <cstdio>
#include <cstdlib>

#include "ipc/latency_db.h"

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "FAIL: %s (line %d)\n", #cond, __LINE__); \
            std::exit(1);                                                  \
        }                                                                  \
    } while (0)

static void TestParseAndLookup() {
    // カスタム区切り子("json")を使う: 生文字列の終端は最初に現れる
    // )json" なので、JSON 側の値に含まれる ")"(例:"Speakers (Realtek)" の
    // 末尾)がデフォルト区切り子 )" と一致して早期終端してしまう事故を防ぐ。
    const char* json = R"json([
        {"renderDevice":"Speakers (Realtek)","captureDevice":"",
         "accessMethod":"wasapi_shared","requestedBufferFrames":64,
         "measuredLatencyMs":6.5,"xrunCount":0,"cpuPercent":3.1},
        {"renderDevice":"Speakers (Realtek)","captureDevice":"",
         "accessMethod":"wasapi_shared","requestedBufferFrames":128,
         "measuredLatencyMs":9.2,"xrunCount":0,"cpuPercent":2.0},
        {"renderDevice":"CABLE Input (VB-Audio)","captureDevice":"",
         "accessMethod":"wasapi_shared","requestedBufferFrames":64,
         "measuredLatencyMs":3.0,"xrunCount":5,"cpuPercent":4.4}
    ])json";

    JsonValue root = JsonValue::Parse(json);
    std::vector<ipc::LatencyMeasurement> db = ipc::ParseLatencyDb(root);
    CHECK(db.size() == 3);
    CHECK(db[0].renderDevice == "Speakers (Realtek)");
    CHECK(db[0].requestedBufferFrames == 64);
    CHECK(db[0].measuredLatencyMs == 6.5);
    CHECK(db[2].xrunCount == 5);

    // 複数マッチのうち xrun=0 の中で最小を選ぶ(6.5 < 9.2)。
    const double lat = ipc::LookupMeasuredLatencyMs(db, "Speakers");
    CHECK(lat == 6.5);

    // xrun が起きた測定しか無いデバイスはヒットしない(未測定扱い = 0.0)。
    const double vbLat = ipc::LookupMeasuredLatencyMs(db, "CABLE Input");
    CHECK(vbLat == 0.0);

    // 一致なし。
    const double none = ipc::LookupMeasuredLatencyMs(db, "Nonexistent Device");
    CHECK(none == 0.0);

    // 空文字列は誰にもマッチしない(全デバイスがヒットする事故を防ぐ)。
    const double empty = ipc::LookupMeasuredLatencyMs(db, "");
    CHECK(empty == 0.0);
}

static void TestEmptyAndMalformedInput() {
    std::vector<ipc::LatencyMeasurement> emptyDb;
    CHECK(ipc::LookupMeasuredLatencyMs(emptyDb, "anything") == 0.0);

    JsonValue notArray = JsonValue::MakeObject();
    std::vector<ipc::LatencyMeasurement> db = ipc::ParseLatencyDb(notArray);
    CHECK(db.empty());
}

int main() {
    TestParseAndLookup();
    TestEmptyAndMalformedInput();
    std::printf("test_latency_db: OK\n");
    return 0;
}
