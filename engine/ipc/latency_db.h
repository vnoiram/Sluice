#pragma once
// latency_db.h : tools/latencybench の実測結果(JSON)を
// DeviceCaps::measuredLatencyMs へ反映するためのオフライン部品(gap 8)。
//
// device_report.h と同じ理由で Windows API に依存しない(main.cpp の
// ファイル読み込み/デバイス名との突き合わせだけが Windows 専用)。
// tools/latencybench --json <path> が出力する配列(1 要素 = 1 測定)を
// パースし、main.cpp がデバイス名の部分一致でルックアップする。

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "ipc/json_value.h"

namespace ipc {

struct LatencyMeasurement {
    std::string renderDevice;   // 検索に使った部分文字列(UTF-8)
    std::string captureDevice;  // 同上(片方向測定なら空)
    std::string accessMethod;   // "wasapi_shared" 等、表示専用
    int requestedBufferFrames = 0;
    double measuredLatencyMs = 0.0;
    uint64_t xrunCount = 0;
};

// tools/latencybench --json が書き出す配列を LatencyMeasurement へ変換する。
// 数値変換に失敗した要素(想定外の形)は読み飛ばす。
inline std::vector<LatencyMeasurement> ParseLatencyDb(const JsonValue& root) {
    std::vector<LatencyMeasurement> out;
    if (root.GetType() != JsonValue::Type::Array) return out;
    out.reserve(root.Items().size());
    for (const auto& item : root.Items()) {
        if (item.GetType() != JsonValue::Type::Object) continue;
        LatencyMeasurement m;
        m.renderDevice = item.At("renderDevice").AsString();
        m.captureDevice = item.At("captureDevice").AsString();
        m.accessMethod = item.At("accessMethod").AsString();
        m.requestedBufferFrames = item.At("requestedBufferFrames").AsInt();
        m.measuredLatencyMs = item.At("measuredLatencyMs").AsNumber();
        m.xrunCount = (uint64_t)item.At("xrunCount").AsNumber();
        out.push_back(std::move(m));
    }
    return out;
}

// deviceNameSubstring を renderDevice/captureDevice に部分一致で含み、
// xrunCount == 0 の測定の中から最小の measuredLatencyMs を返す。
// 該当が無ければ 0.0(= 未測定、DeviceCaps::measuredLatencyMs の既定値と同じ)。
inline double LookupMeasuredLatencyMs(const std::vector<LatencyMeasurement>& db,
                                       const std::string& deviceNameSubstring) {
    if (deviceNameSubstring.empty()) return 0.0;
    bool found = false;
    double best = 0.0;
    for (const auto& m : db) {
        if (m.xrunCount != 0) continue;
        const bool matches =
            (!m.renderDevice.empty() &&
             m.renderDevice.find(deviceNameSubstring) != std::string::npos) ||
            (!m.captureDevice.empty() &&
             m.captureDevice.find(deviceNameSubstring) != std::string::npos);
        if (!matches) continue;
        if (!found || m.measuredLatencyMs < best) {
            best = m.measuredLatencyMs;
            found = true;
        }
    }
    return found ? best : 0.0;
}

}  // namespace ipc
