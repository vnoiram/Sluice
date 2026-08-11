#pragma once
// device_report.h : デバイス状態を IPC の JSON 表現へ変換する純粋関数
// (実装ガイド §5.6「UI に必ず出すもの: 各デバイスのレーン、実効レイテンシ、
// xrun カウンタ、ASRC の現在比」)
//
// Windows API に一切依存しない(DeviceStatus/DeviceCaps は iaudio_device.h
// が既に Windows API 非依存に保っている)。id/name は呼び出し側
// (main.cpp、Windows 専用)が wstring→UTF-8 変換を済ませてから渡すこと
// (WideCharToMultiByte はここでは使わない。オフライン単体テストを可能に
// するため、この関数自体は std::string のみを扱う)。

#include <string>
#include <vector>

#include "device/iaudio_device.h"
#include "ipc/json_value.h"

namespace ipc {

inline const char* LaneToString(Lane lane) { return lane == Lane::RT ? "rt" : "compat"; }

// id/name: UTF-8。backend: "asio"/"wasapi"/"ks"/"process_loopback" 等の固定文字列。
// asrcRatio: エンジン境界(main.cpp の Pipeline::ratioForUi 相当)から渡す
// ASRC 比率。デバイス単体の値ではないため DeviceStatus には含めない
// (engine/device/iaudio_device.h の設計判断、B タスク参照)。
inline JsonValue DeviceReportToJson(const std::string& id, const std::string& name,
                                    const char* backend, bool isCapture,
                                    const DeviceStatus& status, const DeviceCaps& caps,
                                    double asrcRatio) {
    JsonValue v = JsonValue::MakeObject();
    v["id"] = id;
    v["name"] = name;
    v["backend"] = backend;
    v["direction"] = isCapture ? "capture" : "render";
    v["lane"] = LaneToString(status.lane);
    v["recommendedLane"] = LaneToString(caps.recommendedLane);
    v["effectiveLatencyMs"] = status.effectiveLatencySeconds * 1000.0;
    v["bufferSizeFrames"] = (double)status.bufferSizeFrames;
    v["callbackCount"] = (double)status.callbackCount;
    v["underrunCount"] = (double)status.underrunCount;
    v["overrunCount"] = (double)status.overrunCount;
    v["xrunCount"] = (double)(status.underrunCount + status.overrunCount);
    v["resetRequested"] = status.resetRequested;
    v["asrcRatio"] = asrcRatio;
    v["supports64"] = caps.supports64;
    return v;
}

// devices_changed イベント本体({"event":"devices_changed","data":[...]})の
// "data" 部分(デバイス配列)。個々の要素は DeviceReportToJson の結果。
inline JsonValue DeviceReportsToJsonArray(const std::vector<JsonValue>& reports) {
    JsonValue arr = JsonValue::MakeArray();
    for (const auto& r : reports) arr.Push(r);
    return arr;
}

}  // namespace ipc
