// test_device_report_json.cpp : device_report.h のオフライン回帰テスト
// (実装ガイド §5.6)。DeviceStatus/DeviceCaps は Windows API に依存しない
// ため、WIN32 ガード不要で常にビルド・実行できる(test_json_value.cpp と
// 同じパターン)。

#include <cstdio>
#include <cstdlib>

#include "ipc/device_report.h"

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "FAIL: %s (line %d)\n", #cond, __LINE__); \
            std::exit(1);                                                  \
        }                                                                  \
    } while (0)

static void TestBasicFields() {
    DeviceStatus status;
    status.callbackCount = 93750;
    status.underrunCount = 1;
    status.overrunCount = 2;
    status.bufferSizeFrames = 64;
    status.effectiveLatencySeconds = 0.00133;
    status.resetRequested = false;
    status.lane = Lane::RT;

    DeviceCaps caps;
    caps.supports64 = true;
    caps.recommendedLane = Lane::RT;
    caps.measuredLatencyMs = 2.958;

    JsonValue v = ipc::DeviceReportToJson("wasapi:capture:{guid}", "Microphone", "wasapi",
                                          /*isCapture=*/true, status, caps, /*asrcRatio=*/1.0000132);

    CHECK(v.At("id").AsString() == "wasapi:capture:{guid}");
    CHECK(v.At("name").AsString() == "Microphone");
    CHECK(v.At("backend").AsString() == "wasapi");
    CHECK(v.At("direction").AsString() == "capture");
    CHECK(v.At("lane").AsString() == "rt");
    CHECK(v.At("recommendedLane").AsString() == "rt");
    CHECK(v.At("bufferSizeFrames").AsInt() == 64);
    CHECK(v.At("callbackCount").AsNumber() == 93750.0);
    CHECK(v.At("xrunCount").AsNumber() == 3.0);  // underrun(1) + overrun(2)
    CHECK(v.At("resetRequested").AsBool() == false);
    CHECK(v.At("supports64").AsBool() == true);
    CHECK(v.At("measuredLatencyMs").AsNumber() == 2.958);

    // 往復(Dump→Parse)しても壊れないこと。
    JsonValue reparsed = JsonValue::Parse(v.Dump());
    CHECK(reparsed.At("id").AsString() == "wasapi:capture:{guid}");
    CHECK(reparsed.At("xrunCount").AsNumber() == 3.0);
}

static void TestCompatLaneAndRenderDirection() {
    DeviceStatus status;
    status.lane = Lane::Compat;
    DeviceCaps caps;
    caps.recommendedLane = Lane::Compat;

    JsonValue v = ipc::DeviceReportToJson("vbcable:render:0", "CABLE Input", "wasapi",
                                          /*isCapture=*/false, status, caps, /*asrcRatio=*/1.0);
    CHECK(v.At("direction").AsString() == "render");
    CHECK(v.At("lane").AsString() == "compat");
}

static void TestReportsToJsonArray() {
    DeviceStatus status;
    DeviceCaps caps;
    JsonValue a = ipc::DeviceReportToJson("a", "A", "asio", true, status, caps, 1.0);
    JsonValue b = ipc::DeviceReportToJson("b", "B", "asio", false, status, caps, 1.0);

    JsonValue arr = ipc::DeviceReportsToJsonArray({a, b});
    CHECK(arr.GetType() == JsonValue::Type::Array);
    CHECK(arr.Items().size() == 2);
    CHECK(arr.Items()[0].At("id").AsString() == "a");
    CHECK(arr.Items()[1].At("id").AsString() == "b");
}

int main() {
    TestBasicFields();
    TestCompatLaneAndRenderDirection();
    TestReportsToJsonArray();
    std::printf("test_device_report_json: OK\n");
    return 0;
}
