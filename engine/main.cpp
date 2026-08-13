// main.cpp : EngineGraph ベースのミキサー(実装ガイド M1: エンジン統合)
//
// Phase 0(ASIO 1対1パススルー)を置き換え、複数デバイス種別(ASIO/WASAPI/
// DirectKS/プロセスループバック/VB-CABLE/VAC/vasio 仮想 ASIO ブリッジ)を
// 同時に開き、graph/engine_graph.h の EngineGraph(ストリップ/バス/N×M
// ルーティング/RCU グラフ差し替え)へ実際に配線する。
//
// アーキテクチャ(実装ガイド §2.2〜§2.4 に対応):
//   - 各デバイスは自分の RT スレッドで自分のチャンネルごとの SpscRing に
//     読み書きするだけ(device/iaudio_device.h の契約どおり)。例外は vasio
//     ブリッジ(device/vasio_bridge_device.h)で、専用スレッドを持たず
//     マスターの blockCallback から明示的に PumpSharedMemory() される
//     (実装ガイド §8.1「仮想 ASIO のクロックはエンジンに従属する」)。
//   - マスタークロックに選ばれた 1 台だけが SetBlockCallback を登録する。
//     そのコールバックの中で EngineGraph::Process(frames) を呼ぶことが、
//     全ストリップの ASRC 読み出し・全バスの N×M ミックス・全出力リングへの
//     書き込みをまとめて駆動する(§5.4.1 の「マスターコールバック」)。
//     他のデバイスは自分のリングを読み書きするだけで、コールバック登録は
//     不要(engine/device/iaudio_device.h のコメント参照)。vasio ブリッジは
//     常にこの「他のデバイス」側であり、マスターの候補にはならない
//     (自身の独立したクロックを持たないため)。
//   - マスタークロックは master_clock.h により RT Lane のデバイスから選ぶ
//     (§2.3)。RT Lane が 1 台も無い場合は先頭のデバイスにフォールバックし、
//     警告を出す(全滅よりはまし、という割り切り)。
//   - トポロジ変更(ストリップ追加/削除)は EngineGraph を丸ごと作り直し、
//     GraphHandle::Publish() で RCU 差し替えする(§5.4.3)。
//   - デバイスの kAsioResetRequest 相当(DeviceStatus::resetRequested)が
//     立ったら、Phase 0 と同じく「全体を作り直す」最単純対応にする
//     (個々のデバイスだけを差し替える部分再構築は行わない)。
//
// 既知の簡略化(このフェーズの意図的な割り切り):
//   - デバイス構成はプロセス起動時の CLI 引数で固定する。実行中にデバイスを
//     新規追加/差し替えする IPC は無い(add_strip/remove_strip は「既に
//     開いているデバイスのチャンネル」に対するストリップの増減のみ)。
//   - マスタークロック以外の出力デバイスも OutputBoundary/AsrcWriter
//     (graph/engine_graph.h, graph/bus.h, dsp/drift.h)によりドリフト補正
//     される(InputBoundary/AsrcReader の入力側実装と対称)。マスター自身の
//     出力は callback がブロック境界そのものなので ASRC を適用しない
//     (vasio ブリッジはマスターに従属する設計上そもそも独立クロックを
//     持たないため、この対象にもならない)。
//   - EQ/ゲート/コンプ/リミッタの set_param 経由での調整は対応しているが、
//     どの値が「良い」かは呼び出し側の責務(バリデーションは最小限)。

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "crash/crash_handler.h"
#include "device/asio_host.h"
#include "device/iaudio_device.h"
#include "device/ks_device.h"
#include "device/process_loopback_device.h"
#include "device/vac.h"
#include "device/vasio_bridge_device.h"
#include "device/vb_cable.h"
#include "device/wasapi_device.h"
#include "graph/engine_graph.h"
#include "graph/master_clock.h"
#include "ipc/device_report.h"
#include "ipc/latency_db.h"
#include "ipc/pipe_server.h"
#include "rt/rt_alloc_guard.h"
#include "version.h"

namespace {

constexpr double kSampleRate = 48000.0;

// --- 文字コード変換 ---------------------------------------------------------
// engine/ipc/device_report.h・engine/ipc/json_value.h は Windows API 非依存を
// 保つため UTF-8 std::string のみを扱う(オフライン単体テストを可能にする
// ため)。wstring⇔UTF-8 変換はこの Windows 専用ファイル側の責務にする。
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int len =
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), len, nullptr, nullptr);
    return out;
}
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), len);
    return out;
}

// --- gap 8: tools/latencybench --json の結果を DeviceCaps.measuredLatencyMs
//     へ反映する ---------------------------------------------------------------
// 起動時に一度だけ読み込み、以後は BuildDevicesJson/ListDevices からの
// ルックアップだけを行う(実測値ファイルの再読込・ホットリロードは無し。
// 手動で latencybench を再実行してエンジンを再起動する運用を想定)。
std::vector<ipc::LatencyMeasurement> g_latencyDb;

// 実行ファイルと同じディレクトリの latencybench-results.json を既定パスとする
// (latencybench.exe --json <path> の出力をそのまま指せるよう、明示パスは
// --latency-db で上書きできる)。見つからなければ静かに空のまま(測定値
// 無しは既存の「未測定=0.0」動作と等価)。
std::wstring DefaultLatencyDbPath() {
    wchar_t exePath[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return L"";
    std::wstring path(exePath, len);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L"";
    return path.substr(0, slash + 1) + L"latencybench-results.json";
}

// ListDevices() の表示行に付け足す " measured=X.XXms" 断片(未測定なら空文字)。
std::wstring FormatMeasuredLatencySuffix(double measuredLatencyMs) {
    if (measuredLatencyMs <= 0.0) return L"";
    wchar_t buf[48];
    swprintf(buf, 48, L", measured=%.2fms", measuredLatencyMs);
    return buf;
}

void LoadLatencyDb(const std::wstring& explicitPath) {
    const std::wstring path = explicitPath.empty() ? DefaultLatencyDbPath() : explicitPath;
    if (path.empty()) return;
    // 中身は UTF-8 のテキスト(tools/latencybench --json が素の std::ofstream で
    // バイト列として書き出したもの)なので、ワイド文字ストリームは使わず
    // バイト列のまま読む(std::ifstream の wstring パス受け取りは csvPath と
    // 同じ MSVC 拡張)。JsonValue::Parse は UTF-8 std::string をそのまま扱える。
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return;
    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string text = buf.str();
    try {
        JsonValue root = JsonValue::Parse(text);
        g_latencyDb = ipc::ParseLatencyDb(root);
        wprintf(L"loaded %zu latency measurement(s) from %s\n", g_latencyDb.size(), path.c_str());
    } catch (const std::exception&) {
        // 壊れた/古い形式のファイルは無視する(実測値無し運用にフォールバック)。
    }
}

// ===========================================================================
// デバイス構成(CLI から組み立てる「起動時に何を開くか」の指定)
// ===========================================================================
struct DeviceSpec {
    enum class Backend { Asio, Wasapi, Ks, Loopback, Vasio } backend = Backend::Wasapi;
    bool isCapture = true;
    int index = -1;             // asio ドライバ index / ks device index(--list 参照)
    std::wstring endpointId;    // wasapi: 明示的なエンドポイント ID(VB-CABLE/VAC 自動検出用)
    std::wstring displayName;   // 明示的な表示名(空なら列挙結果の名前を使う)
    DWORD pid = 0;               // loopback 用
    std::string backendLabel;   // 表示上の backend 名を上書きしたい場合("vbcable"/"vac")
    std::wstring vasioInstanceId = L"0";  // gap 11: vasio 用。--vasio-instance 参照
};

// ===========================================================================
// 開いたデバイス 1 台分
// ===========================================================================
// isCapture のような単一方向フラグは持たない。ASIO/WASAPI/KS/
// プロセスループバックは片方向しか実装しない(CaptureRing/RenderRing の
// 片方が常に nullptr を返す)が、vasio ブリッジ(device/vasio_bridge_device.h)
// は両方向を同時に提供する。実際にどちらの方向を持つかは常に
// ProbeChannelCount() で判定する(下記)。
struct DeviceEntry {
    std::unique_ptr<IAudioDevice> device;
    std::string backend;
    std::string name;   // UTF-8
};

int ProbeChannelCount(IAudioDevice* dev, bool isCapture) {
    int c = 0;
    while (isCapture ? dev->CaptureRing(c) != nullptr : dev->RenderRing(c) != nullptr) ++c;
    return c;
}

// ===========================================================================
// EngineGraph のトポロジ指定(制御スレッドが保持する「正」の状態)
// ===========================================================================
struct BoundarySpec {
    int deviceIndex = -1;  // devices_ 内のキャプチャデバイスの index
};
struct StripSpec {
    int boundaryIndex = -1;
    int deviceIndex = -1;
    int channel = -1;
    StripParams params;
};
// OutputBoundarySpec: BoundarySpec と対称。非マスターのレンダーデバイス
// 1 台につき 1 つ(実装ガイド §5.1 の考え方を出力側に適用したもの、
// graph/engine_graph.h の OutputBoundary 参照)。マスターデバイスの出力は
// ここに含めない(ASRC を適用しない)。
struct OutputBoundarySpec {
    int deviceIndex = -1;  // devices_ 内のレンダーデバイスの index
};
struct BusSpec {
    int deviceIndex = -1;  // devices_ 内のレンダーデバイスの index
    int channel = -1;
    BusParams params;
    int outputBoundaryIndex = -1;  // -1 = マスターバス(ASRC 無し)
};

struct MixerState {
    std::vector<DeviceEntry> devices;
    std::vector<BoundarySpec> boundaries;
    std::vector<StripSpec> strips;
    std::vector<OutputBoundarySpec> outputBoundaries;
    std::vector<BusSpec> buses;

    std::unique_ptr<GraphHandle> graphHandle;
    EngineGraph* controlGraph = nullptr;  // 直近に Publish() した生ポインタ(制御スレッド専用)
    IAudioDevice* masterDevice = nullptr;
    int maxBlockFrames = 64;

    // vasio ブリッジ(devices 内にも通常どおり存在する)への型付きアクセス。
    // PumpSharedMemory() は IAudioDevice の一般契約に無いメソッドなので、
    // master の blockCallback から呼ぶために別途保持する(実装ガイド §8.1)。
    std::vector<vasiobridge::VasioBridgeDevice*> vasioBridges;
};

// EngineGraph を現在のトポロジ指定から丸ごと作り直し、RCU 差し替えする
// (実装ガイド §5.4.3)。初回呼び出し(graphHandle が未生成)では待たずに
// 即座に公開され、2 回目以降(デバイス稼働中)は RT が新グラフを Acquire()
// するまで GraphHandle::Publish() が待つ。
void RebuildEngineGraph(MixerState& st) {
    std::vector<InputBoundary> boundaries;
    boundaries.reserve(st.boundaries.size());
    for (const auto& b : st.boundaries)
        boundaries.emplace_back(*st.devices[(size_t)b.deviceIndex].device->CaptureRing(0));

    std::vector<OutputBoundary> outputBoundaries;
    outputBoundaries.reserve(st.outputBoundaries.size());
    for (const auto& ob : st.outputBoundaries)
        outputBoundaries.emplace_back(*st.devices[(size_t)ob.deviceIndex].device->RenderRing(0));

    std::vector<StripRuntime> strips;
    strips.reserve(st.strips.size());
    for (const auto& s : st.strips) {
        DeviceEntry& entry = st.devices[(size_t)s.deviceIndex];
        SpscRing<float>* ring = entry.device->CaptureRing(s.channel);
        const Lane lane = entry.device->Status().lane;
        strips.emplace_back(*ring, st.maxBlockFrames, (float)kSampleRate, s.params,
                            s.boundaryIndex, lane);
    }

    std::vector<BusRuntime> buses;
    buses.reserve(st.buses.size());
    for (const auto& b : st.buses) {
        DeviceEntry& entry = st.devices[(size_t)b.deviceIndex];
        std::vector<SpscRing<float>*> outputs{entry.device->RenderRing(b.channel)};
        const Lane lane = entry.device->Status().lane;
        buses.emplace_back(st.maxBlockFrames, (float)kSampleRate, std::move(outputs), b.params,
                           b.outputBoundaryIndex, lane);
    }

    auto newGraph = std::make_unique<EngineGraph>(std::move(boundaries), std::move(strips),
                                                   std::move(buses), std::move(outputBoundaries));
    EngineGraph* raw = newGraph.get();
    if (!st.graphHandle) st.graphHandle = std::make_unique<GraphHandle>();
    st.graphHandle->Publish(std::move(newGraph));
    st.controlGraph = raw;
}

// --- 1 台のデバイスを DeviceSpec からオープンする -----------------------------
bool OpenOneDevice(const DeviceSpec& spec, const std::vector<asiohost::DriverInfo>& asioDrivers,
                   const DeviceStreamConfig& config, DeviceEntry* outEntry,
                   std::wstring* errorOut) {
    switch (spec.backend) {
    case DeviceSpec::Backend::Asio: {
        if (spec.index < 0 || (size_t)spec.index >= asioDrivers.size()) {
            *errorOut = L"ASIO driver index out of range";
            return false;
        }
        auto dev = std::make_unique<asiohost::AsioDevice>(asioDrivers[(size_t)spec.index],
                                                           spec.isCapture);
        if (!dev->Open(config, errorOut)) return false;
        outEntry->name = WideToUtf8(asioDrivers[(size_t)spec.index].name);
        outEntry->backend = "asio";
        outEntry->device = std::move(dev);
        return true;
    }
    case DeviceSpec::Backend::Wasapi: {
        std::wstring endpointId = spec.endpointId;
        std::wstring displayName = spec.displayName;
        if (endpointId.empty() && spec.index >= 0) {
            auto endpoints = wasapi::EnumerateEndpoints(spec.isCapture);
            if ((size_t)spec.index >= endpoints.size()) {
                *errorOut = L"WASAPI endpoint index out of range";
                return false;
            }
            endpointId = endpoints[(size_t)spec.index].id;
            if (displayName.empty()) displayName = endpoints[(size_t)spec.index].name;
        }
        auto dev = std::make_unique<wasapi::WasapiDevice>(endpointId, spec.isCapture);
        if (!dev->Open(config, errorOut)) return false;
        outEntry->name = displayName.empty() ? "WASAPI" : WideToUtf8(displayName);
        outEntry->backend = spec.backendLabel.empty() ? "wasapi" : spec.backendLabel;
        outEntry->device = std::move(dev);
        return true;
    }
    case DeviceSpec::Backend::Ks: {
        auto ksDevices = ks::EnumerateKsAudioDevices();
        if (spec.index < 0 || (size_t)spec.index >= ksDevices.size()) {
            *errorOut = L"KS device index out of range";
            return false;
        }
        auto dev = std::make_unique<ks::KsDevice>(ksDevices[(size_t)spec.index], spec.isCapture);
        if (!dev->Open(config, errorOut)) return false;
        outEntry->name = WideToUtf8(ksDevices[(size_t)spec.index].friendlyName);
        outEntry->backend = "ks";
        outEntry->device = std::move(dev);
        return true;
    }
    case DeviceSpec::Backend::Loopback: {
        auto dev = std::make_unique<wasapi::ProcessLoopbackDevice>(spec.pid,
                                                                    /*includeChildren=*/true);
        if (!dev->Open(config, errorOut)) return false;
        outEntry->name = "PID " + std::to_string(spec.pid);
        outEntry->backend = "process_loopback";
        outEntry->device = std::move(dev);
        return true;
    }
    case DeviceSpec::Backend::Vasio: {
        // vasio.dll(仮想 ASIO ドライバ)との共有メモリ接続。DAW→エンジンの
        // capture 方向と エンジン→DAW の render 方向を同時に提供する
        // (実装ガイド §8.1)。spec.index/endpointId は参照しない。
        // gap 11: instanceId は既定 "0"(単一インスタンス運用、導入前と
        // 完全に後方互換)。複数インスタンスを使う場合は --vasio-instance
        // で明示し、対応する DAW プロセス側は環境変数
        // SLUICE_VASIO_INSTANCE に同じ値を設定しておく必要がある
        // (vasio/vasio_driver.cpp 参照)。
        auto dev = std::make_unique<vasiobridge::VasioBridgeDevice>(spec.vasioInstanceId);
        if (!dev->Open(config, errorOut)) return false;
        outEntry->name = "vasio (virtual ASIO bridge)";
        outEntry->backend = "vasio";
        outEntry->device = std::move(dev);
        return true;
    }
    }
    *errorOut = L"unknown device backend";
    return false;
}

// --- 全デバイスを開き、境界/ストリップ/バス構成とマスタークロックを決めて
//     EngineGraph を構築・公開し、Start() まで行う。失敗時は途中まで開いた
//     デバイスを Close() してから nullptr を返す。 -----------------------------
std::unique_ptr<MixerState> OpenAndBuild(const std::vector<DeviceSpec>& specs,
                                         const DeviceStreamConfig& baseConfig, bool autoRoute,
                                         std::wstring* errorOut) {
    auto st = std::make_unique<MixerState>();
    auto asioDrivers = asiohost::EnumerateDrivers();

    for (const auto& spec : specs) {
        DeviceEntry entry;
        if (!OpenOneDevice(spec, asioDrivers, baseConfig, &entry, errorOut)) {
            for (auto& e : st->devices) e.device->Close();
            return nullptr;
        }
        // vasio ブリッジは PumpSharedMemory() という IAudioDevice の一般契約に
        // 無いメソッドを持つため、型付きの参照も別途保持しておく(devices には
        // 通常どおり IAudioDevice* として入る)。
        if (auto* bridge = dynamic_cast<vasiobridge::VasioBridgeDevice*>(entry.device.get()))
            st->vasioBridges.push_back(bridge);
        st->devices.push_back(std::move(entry));
    }

    // 境界: チャンネル数が 1 以上のキャプチャ方向を持つデバイス 1 台につき 1 つ
    // (実装ガイド §5.4「InputBoundary」)。RebuildEngineGraph は境界の
    // 代表チャンネルとして CaptureRing(0) を無条件に参照するため、
    // チャンネル 0 が存在しないデバイス(想定外だが理論上あり得る)を
    // 境界に加えるとクラッシュする。ここで弾いておく。
    // ストリップ: (キャプチャ方向を持つデバイス, チャンネル) の組ごとに 1 つ。
    // ASIO/WASAPI/KS/プロセスループバックは片方向のみ実装するため
    // ProbeChannelCount は自動的に 0 を返す(isCapture フラグでの分岐は
    // 不要。vasio ブリッジのように両方向を同時に持つデバイスも自然に扱える)。
    for (size_t i = 0; i < st->devices.size(); ++i) {
        IAudioDevice* dev = st->devices[i].device.get();
        const int channels = ProbeChannelCount(dev, /*isCapture=*/true);
        if (channels <= 0) continue;
        BoundarySpec b;
        b.deviceIndex = (int)i;
        const int boundaryIndex = (int)st->boundaries.size();
        st->boundaries.push_back(b);
        for (int ch = 0; ch < channels; ++ch) {
            StripSpec s;
            s.boundaryIndex = boundaryIndex;
            s.deviceIndex = (int)i;
            s.channel = ch;
            st->strips.push_back(s);
        }
    }

    // マスタークロック選定(実装ガイド §2.3): RT Lane のデバイスから選ぶ。
    // 出力デバイスを優先する(§5.4.1 の「マスターコールバック」は本来
    // RenderRing 読み出し直前に発火するもので、出力側が自然)。vasio ブリッジ
    // は Lane::RT を報告するが**候補から除外する**: vasio 自身は独立した
    // クロックを持たず「エンジンのマスタークロックに従属する」設計
    // (実装ガイド §8.1 手順4)なので、それ自身をマスターに選ぶと駆動元が
    // 無くなり循環する。
    // (バスの構築より前に選ぶ必要がある: マスター以外のレンダーデバイスに
    // だけ OutputBoundarySpec を割り当てるため。)
    std::vector<IAudioDevice*> allPtrs;
    allPtrs.reserve(st->devices.size());
    for (auto& e : st->devices) {
        if (dynamic_cast<vasiobridge::VasioBridgeDevice*>(e.device.get())) continue;
        allPtrs.push_back(e.device.get());
    }
    const std::vector<IAudioDevice*> rtCandidates = engine::SelectMasterClockCandidates(allPtrs);

    IAudioDevice* master = nullptr;
    for (IAudioDevice* d : rtCandidates) {
        if (ProbeChannelCount(d, /*isCapture=*/false) > 0) {
            master = d;
            break;
        }
    }
    if (!master && !rtCandidates.empty()) master = rtCandidates.front();
    if (!master && !allPtrs.empty()) {
        wprintf(L"warning: no RT-lane device available; falling back to the first opened "
                L"device as master clock (guide %s: latency/robustness will suffer)\n",
                L"§2.3");
        master = allPtrs.front();
    }
    if (!master) {
        *errorOut = L"no non-vasio devices available to act as master clock";
        for (auto& e : st->devices) e.device->Close();
        return nullptr;
    }

    // バス: (レンダー方向を持つデバイス, チャンネル) の組ごとに 1 つ
    // (ストリップと対称)。マスター以外のレンダーデバイスは、デバイス
    // 1 台につき 1 つの OutputBoundarySpec を割り当てる(非マスター出力側
    // の ASRC/ドリフト補正、実装ガイド §5.1 の考え方を出力側にも適用)。
    for (size_t di = 0; di < st->devices.size(); ++di) {
        IAudioDevice* dev = st->devices[di].device.get();
        const int channels = ProbeChannelCount(dev, /*isCapture=*/false);
        if (channels <= 0) continue;
        int outputBoundaryIndex = -1;
        if (dev != master) {
            OutputBoundarySpec ob;
            ob.deviceIndex = (int)di;
            outputBoundaryIndex = (int)st->outputBoundaries.size();
            st->outputBoundaries.push_back(ob);
        }
        for (int ch = 0; ch < channels; ++ch) {
            BusSpec b;
            b.deviceIndex = (int)di;
            b.channel = ch;
            b.outputBoundaryIndex = outputBoundaryIndex;
            st->buses.push_back(b);
        }
    }

    if (st->buses.empty()) {
        *errorOut = L"no output (render) devices configured";
        for (auto& e : st->devices) e.device->Close();
        return nullptr;
    }

    // --auto-route: 全ストリップを全バスへ 0dB で送る(実装ガイド既定の
    // 「全ルーティングgainはミュート」を上書きする、お試し用の便宜フラグ)。
    if (autoRoute) {
        for (auto& s : st->strips)
            for (size_t bi = 0; bi < st->buses.size() && bi < StripParams::kMaxBuses; ++bi)
                s.params.routingGain[bi] = 0.0f;
    }

    const long masterBuf = master->Status().bufferSizeFrames;
    st->maxBlockFrames = std::max(64, (int)masterBuf);

    RebuildEngineGraph(*st);

    MixerState* stPtr = st.get();
    master->SetBlockCallback([stPtr](int frames) {
        // 実装ガイド §3.2: 開発ビルド(SLUICE_RT_ALLOC_GUARD、CMakeLists.txt
        // の Debug 構成)ではここから抜けるまでのアロケーションを検出する。
        // Release ビルドでは RtGuard は空になりオーバーヘッドが無い。
        rtguard::RtGuard rtGuard;
        EngineGraph* g = stPtr->graphHandle->Acquire();
        if (g) g->Process(frames);
        // vasio ブリッジは自前の RT スレッドを持たないので、マスターの
        // ブロックごとに明示的にポンプする(実装ガイド §8.1 手順4)。
        // graph->Process() の**後**に呼ぶことで、このブロックで
        // BusRuntime が書いたばかりの FromEngine データを送り出せる。
        for (auto* bridge : stPtr->vasioBridges) bridge->PumpSharedMemory(frames);
    });
    st->masterDevice = master;

    for (auto& e : st->devices) e.device->Start();

    return st;
}

void StopAndCloseAll(MixerState& st) {
    for (auto& e : st.devices) e.device->Stop();
    for (auto& e : st.devices) e.device->Close();
}

// ===========================================================================
// DSP パラメータ ⇔ JSON(実装ガイド §5.5 のパラメータを IPC 越しに調整可能に
// する。engine/ipc/json_value.h は浅いネストのオブジェクトを扱える)
// ===========================================================================
JsonValue GateParamsToJson(const GateParams& p) {
    JsonValue v = JsonValue::MakeObject();
    v["enabled"] = p.enabled;
    v["thresholdDb"] = p.thresholdDb;
    v["hysteresisDb"] = p.hysteresisDb;
    v["attackMs"] = p.attackMs;
    v["releaseMs"] = p.releaseMs;
    return v;
}
void GateParamsFromJson(const JsonValue& v, GateParams* p) {
    if (v.Has("enabled")) p->enabled = v.At("enabled").AsBool(p->enabled);
    if (v.Has("thresholdDb")) p->thresholdDb = (float)v.At("thresholdDb").AsNumber(p->thresholdDb);
    if (v.Has("hysteresisDb"))
        p->hysteresisDb = (float)v.At("hysteresisDb").AsNumber(p->hysteresisDb);
    if (v.Has("attackMs")) p->attackMs = (float)v.At("attackMs").AsNumber(p->attackMs);
    if (v.Has("releaseMs")) p->releaseMs = (float)v.At("releaseMs").AsNumber(p->releaseMs);
}

JsonValue EqParamsToJson(const EqParams& p) {
    JsonValue v = JsonValue::MakeObject();
    v["enabled"] = p.enabled;
    v["lowShelfFreq"] = p.lowShelfFreq;
    v["lowShelfGainDb"] = p.lowShelfGainDb;
    v["peak1Freq"] = p.peak1Freq;
    v["peak1GainDb"] = p.peak1GainDb;
    v["peak1Q"] = p.peak1Q;
    v["peak2Freq"] = p.peak2Freq;
    v["peak2GainDb"] = p.peak2GainDb;
    v["peak2Q"] = p.peak2Q;
    v["highShelfFreq"] = p.highShelfFreq;
    v["highShelfGainDb"] = p.highShelfGainDb;
    return v;
}
void EqParamsFromJson(const JsonValue& v, EqParams* p) {
    if (v.Has("enabled")) p->enabled = v.At("enabled").AsBool(p->enabled);
    if (v.Has("lowShelfFreq")) p->lowShelfFreq = (float)v.At("lowShelfFreq").AsNumber(p->lowShelfFreq);
    if (v.Has("lowShelfGainDb"))
        p->lowShelfGainDb = (float)v.At("lowShelfGainDb").AsNumber(p->lowShelfGainDb);
    if (v.Has("peak1Freq")) p->peak1Freq = (float)v.At("peak1Freq").AsNumber(p->peak1Freq);
    if (v.Has("peak1GainDb")) p->peak1GainDb = (float)v.At("peak1GainDb").AsNumber(p->peak1GainDb);
    if (v.Has("peak1Q")) p->peak1Q = (float)v.At("peak1Q").AsNumber(p->peak1Q);
    if (v.Has("peak2Freq")) p->peak2Freq = (float)v.At("peak2Freq").AsNumber(p->peak2Freq);
    if (v.Has("peak2GainDb")) p->peak2GainDb = (float)v.At("peak2GainDb").AsNumber(p->peak2GainDb);
    if (v.Has("peak2Q")) p->peak2Q = (float)v.At("peak2Q").AsNumber(p->peak2Q);
    if (v.Has("highShelfFreq"))
        p->highShelfFreq = (float)v.At("highShelfFreq").AsNumber(p->highShelfFreq);
    if (v.Has("highShelfGainDb"))
        p->highShelfGainDb = (float)v.At("highShelfGainDb").AsNumber(p->highShelfGainDb);
}

JsonValue CompParamsToJson(const CompParams& p) {
    JsonValue v = JsonValue::MakeObject();
    v["enabled"] = p.enabled;
    v["thresholdDb"] = p.thresholdDb;
    v["ratio"] = p.ratio;
    v["attackMs"] = p.attackMs;
    v["releaseMs"] = p.releaseMs;
    v["makeupDb"] = p.makeupDb;
    v["lookaheadMs"] = p.lookaheadMs;
    return v;
}
void CompParamsFromJson(const JsonValue& v, CompParams* p) {
    if (v.Has("enabled")) p->enabled = v.At("enabled").AsBool(p->enabled);
    if (v.Has("thresholdDb")) p->thresholdDb = (float)v.At("thresholdDb").AsNumber(p->thresholdDb);
    if (v.Has("ratio")) p->ratio = (float)v.At("ratio").AsNumber(p->ratio);
    if (v.Has("attackMs")) p->attackMs = (float)v.At("attackMs").AsNumber(p->attackMs);
    if (v.Has("releaseMs")) p->releaseMs = (float)v.At("releaseMs").AsNumber(p->releaseMs);
    if (v.Has("makeupDb")) p->makeupDb = (float)v.At("makeupDb").AsNumber(p->makeupDb);
    if (v.Has("lookaheadMs")) p->lookaheadMs = (float)v.At("lookaheadMs").AsNumber(p->lookaheadMs);
}

JsonValue LimiterParamsToJson(const LimiterParams& p) {
    JsonValue v = JsonValue::MakeObject();
    v["enabled"] = p.enabled;
    v["ceilingDb"] = p.ceilingDb;
    v["kneeDb"] = p.kneeDb;
    return v;
}
void LimiterParamsFromJson(const JsonValue& v, LimiterParams* p) {
    if (v.Has("enabled")) p->enabled = v.At("enabled").AsBool(p->enabled);
    if (v.Has("ceilingDb")) p->ceilingDb = (float)v.At("ceilingDb").AsNumber(p->ceilingDb);
    if (v.Has("kneeDb")) p->kneeDb = (float)v.At("kneeDb").AsNumber(p->kneeDb);
}

// ===========================================================================
// IPC 用 JSON 構築(実装ガイド §5.6)
// ===========================================================================
JsonValue BuildDevicesJson(const MixerState& st) {
    std::vector<JsonValue> reports;
    reports.reserve(st.devices.size());
    for (size_t i = 0; i < st.devices.size(); ++i) {
        const DeviceEntry& e = st.devices[i];
        const DeviceStatus status = e.device->Status();
        DeviceCaps caps = e.device->Probe(kSampleRate);
        // gap 8: tools/latencybench --json の実測値があれば反映する
        // (Probe() 自体は各バックエンドとも常に未測定=0.0 のまま)。
        caps.measuredLatencyMs = ipc::LookupMeasuredLatencyMs(g_latencyDb, e.name);
        const bool isMaster = (e.device.get() == st.masterDevice);

        // 片方向デバイス(ASIO/WASAPI/KS/プロセスループバック)はどちらか
        // 一方だけ、vasio ブリッジのように両方向を持つデバイスは 2 件
        // (in と out)報告する。id は "backend:方向:devices[]のindex"。
        const bool hasCapture = ProbeChannelCount(e.device.get(), /*isCapture=*/true) > 0;
        const bool hasRender = ProbeChannelCount(e.device.get(), /*isCapture=*/false) > 0;

        if (hasCapture) {
            double ratio = 1.0;
            if (st.controlGraph) {
                for (size_t bi = 0; bi < st.boundaries.size(); ++bi) {
                    if (st.boundaries[bi].deviceIndex == (int)i) {
                        ratio = st.controlGraph->BoundaryRatioForUi(bi);
                        break;
                    }
                }
            }
            const std::string id = e.backend + ":in:" + std::to_string(i);
            JsonValue report = ipc::DeviceReportToJson(id, e.name, e.backend.c_str(),
                                                       /*isCapture=*/true, status, caps, ratio);
            report["isMasterClock"] = isMaster;
            reports.push_back(report);
        }
        if (hasRender) {
            // マスターデバイスの出力には ASRC が無い(bus.h の
            // outputBoundaryIndex == -1)ので 1.0 のまま。それ以外は
            // OutputBoundary の直近比を参照する(キャプチャ方向の
            // BoundaryRatioForUi と対称)。
            double ratio = 1.0;
            if (st.controlGraph) {
                for (size_t obi = 0; obi < st.outputBoundaries.size(); ++obi) {
                    if (st.outputBoundaries[obi].deviceIndex == (int)i) {
                        ratio = st.controlGraph->OutputBoundaryRatioForUi(obi);
                        break;
                    }
                }
            }
            const std::string id = e.backend + ":out:" + std::to_string(i);
            JsonValue report = ipc::DeviceReportToJson(id, e.name, e.backend.c_str(),
                                                       /*isCapture=*/false, status, caps, ratio);
            report["isMasterClock"] = isMaster;
            reports.push_back(report);
        }
    }
    return ipc::DeviceReportsToJsonArray(reports);
}

JsonValue BuildTopologyJson(const MixerState& st) {
    JsonValue strips = JsonValue::MakeArray();
    for (size_t i = 0; i < st.strips.size(); ++i) {
        const StripSpec& s = st.strips[i];
        JsonValue v = JsonValue::MakeObject();
        v["index"] = (int)i;
        v["deviceIndex"] = s.deviceIndex;
        v["channel"] = s.channel;
        v["boundaryIndex"] = s.boundaryIndex;
        v["gainDb"] = s.params.gainDb;
        v["mute"] = s.params.mute;
        v["solo"] = s.params.solo;
        JsonValue routing = JsonValue::MakeArray();
        for (size_t bi = 0; bi < st.buses.size(); ++bi) routing.Push(s.params.routingGain[bi]);
        v["routingGain"] = routing;
        v["gate"] = GateParamsToJson(s.params.gate);
        v["eq"] = EqParamsToJson(s.params.eq);
        v["comp"] = CompParamsToJson(s.params.comp);
        strips.Push(v);
    }
    JsonValue buses = JsonValue::MakeArray();
    for (size_t i = 0; i < st.buses.size(); ++i) {
        const BusSpec& b = st.buses[i];
        JsonValue v = JsonValue::MakeObject();
        v["index"] = (int)i;
        v["deviceIndex"] = b.deviceIndex;
        v["channel"] = b.channel;
        v["gainDb"] = b.params.gainDb;
        v["limiter"] = LimiterParamsToJson(b.params.limiter);
        buses.Push(v);
    }
    JsonValue root = JsonValue::MakeObject();
    root["strips"] = strips;
    root["buses"] = buses;
    return root;
}

void PrintStats(const MixerState& st) {
    wprintf(L"devices=%zu strips=%zu buses=%zu\n", st.devices.size(), st.strips.size(),
            st.buses.size());
    for (size_t i = 0; i < st.devices.size(); ++i) {
        const DeviceEntry& e = st.devices[i];
        const DeviceStatus s = e.device->Status();
        const bool hasCapture = ProbeChannelCount(e.device.get(), /*isCapture=*/true) > 0;
        const bool hasRender = ProbeChannelCount(e.device.get(), /*isCapture=*/false) > 0;
        const wchar_t* dir = (hasCapture && hasRender) ? L"in+out" : (hasCapture ? L"in" : L"out");
        wprintf(L"  [%zu]%s %s/%s \"%s\"  xrun=%llu  latency=%.2fms  lane=%s\n", i,
                (e.device.get() == st.masterDevice) ? L" *master*" : L"",
                Utf8ToWide(e.backend).c_str(), dir, Utf8ToWide(e.name).c_str(),
                (unsigned long long)(s.underrunCount + s.overrunCount),
                s.effectiveLatencySeconds * 1000.0, s.lane == Lane::RT ? L"RT" : L"Compat");
    }
}

// --- --list: 開かずに列挙だけする(実装ガイド §2.5 tools/devprobe 的な用途を
//     ここに内蔵している。専用 CLI は将来課題)。 -----------------------------
void ListDevices() {
    wprintf(L"ASIO drivers (--asio-in/--asio-out <idx>):\n");
    auto asioDrivers = asiohost::EnumerateDrivers();
    for (size_t i = 0; i < asioDrivers.size(); ++i) {
        asiohost::AsioDevice probe(asioDrivers[i], /*isInput=*/true);
        DeviceCaps caps = probe.Probe(kSampleRate);
        caps.measuredLatencyMs =
            ipc::LookupMeasuredLatencyMs(g_latencyDb, WideToUtf8(asioDrivers[i].name));
        wprintf(L"  [%zu] %s  (supports64=%s, lane=%s%s)\n", i, asioDrivers[i].name.c_str(),
                caps.supports64 ? L"yes" : L"no",
                caps.recommendedLane == Lane::RT ? L"RT" : L"Compat",
                FormatMeasuredLatencySuffix(caps.measuredLatencyMs).c_str());
    }

    wprintf(L"\nWASAPI capture endpoints (--wasapi-in <idx>):\n");
    auto wasapiIn = wasapi::EnumerateEndpoints(/*isCapture=*/true);
    for (size_t i = 0; i < wasapiIn.size(); ++i) {
        wasapi::WasapiDevice probe(wasapiIn[i].id, /*isCapture=*/true);
        DeviceCaps caps = probe.Probe(kSampleRate);
        caps.measuredLatencyMs =
            ipc::LookupMeasuredLatencyMs(g_latencyDb, WideToUtf8(wasapiIn[i].name));
        wprintf(L"  [%zu] %s  (supports64=%s, lane=%s%s)\n", i, wasapiIn[i].name.c_str(),
                caps.supports64 ? L"yes" : L"no",
                caps.recommendedLane == Lane::RT ? L"RT" : L"Compat",
                FormatMeasuredLatencySuffix(caps.measuredLatencyMs).c_str());
    }

    wprintf(L"\nWASAPI render endpoints (--wasapi-out <idx>):\n");
    auto wasapiOut = wasapi::EnumerateEndpoints(/*isCapture=*/false);
    for (size_t i = 0; i < wasapiOut.size(); ++i) {
        wasapi::WasapiDevice probe(wasapiOut[i].id, /*isCapture=*/false);
        DeviceCaps caps = probe.Probe(kSampleRate);
        caps.measuredLatencyMs =
            ipc::LookupMeasuredLatencyMs(g_latencyDb, WideToUtf8(wasapiOut[i].name));
        wprintf(L"  [%zu] %s  (supports64=%s, lane=%s%s)\n", i, wasapiOut[i].name.c_str(),
                caps.supports64 ? L"yes" : L"no",
                caps.recommendedLane == Lane::RT ? L"RT" : L"Compat",
                FormatMeasuredLatencySuffix(caps.measuredLatencyMs).c_str());
    }

    wprintf(L"\nKS devices (--ks-in/--ks-out <idx>, direction determined by pin discovery):\n");
    auto ksDevices = ks::EnumerateKsAudioDevices();
    for (size_t i = 0; i < ksDevices.size(); ++i)
        wprintf(L"  [%zu] %s\n", i, ksDevices[i].friendlyName.c_str());

    auto vb = wasapi::DetectVbCable();
    wprintf(L"\nVB-CABLE (--vbcable-in / --vbcable-out): input=%s output=%s\n",
            vb.virtualInput ? L"found" : L"not found",
            vb.virtualOutput ? L"found" : L"not found");

    auto vac = wasapi::DetectVac();
    wprintf(L"VAC lines (--vac-line <n>):");
    if (vac.empty()) {
        wprintf(L" none found\n");
    } else {
        for (const auto& line : vac) wprintf(L" %d", line.lineNumber);
        wprintf(L"\n");
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // 起動直後、他の何より先にクラッシュダンプ収集を有効化する(実装ガイド §5.7)。
    crashhandler::Install();
    wprintf(L"sluice-engine %s\n", version::kStringW);

    // --- 引数処理 -------------------------------------------------------------
    std::vector<DeviceSpec> specs;
    bool listOnly = false;
    bool lowLatency = false;
    bool autoRoute = false;
    int reqChannels = 2;
    std::wstring latencyDbPath;  // gap 8: 既定は実行ファイル隣接の latencybench-results.json

    auto nextInt = [&](int i, int def) -> int {
        return (i + 1 < argc) ? _wtoi(argv[i + 1]) : def;
    };
    auto nextWStr = [&](int i, const std::wstring& def) -> std::wstring {
        return (i + 1 < argc) ? std::wstring(argv[i + 1]) : def;
    };

    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--list") {
            listOnly = true;
        } else if (a == L"--low-latency") {
            lowLatency = true;
        } else if (a == L"--auto-route") {
            autoRoute = true;
        } else if (a == L"--channels") {
            reqChannels = nextInt(i, reqChannels);
            ++i;
        } else if (a == L"--latency-db") {
            // gap 8: tools/latencybench --json の出力パスを明示指定する
            // (省略時は実行ファイル隣接の latencybench-results.json を試す)。
            latencyDbPath = (i + 1 < argc) ? argv[i + 1] : L"";
            ++i;
        } else if (a == L"--asio-in" || a == L"--asio-out") {
            DeviceSpec s;
            s.backend = DeviceSpec::Backend::Asio;
            s.isCapture = (a == L"--asio-in");
            s.index = nextInt(i, -1);
            ++i;
            specs.push_back(s);
        } else if (a == L"--wasapi-in" || a == L"--wasapi-out") {
            DeviceSpec s;
            s.backend = DeviceSpec::Backend::Wasapi;
            s.isCapture = (a == L"--wasapi-in");
            s.index = nextInt(i, -1);
            ++i;
            specs.push_back(s);
        } else if (a == L"--ks-in" || a == L"--ks-out") {
            DeviceSpec s;
            s.backend = DeviceSpec::Backend::Ks;
            s.isCapture = (a == L"--ks-in");
            s.index = nextInt(i, -1);
            ++i;
            specs.push_back(s);
        } else if (a == L"--loopback-pid") {
            DeviceSpec s;
            s.backend = DeviceSpec::Backend::Loopback;
            s.isCapture = true;
            s.pid = (DWORD)nextInt(i, 0);
            ++i;
            specs.push_back(s);
        } else if (a == L"--vasio") {
            // vasio.dll(vasio/vasio_driver.cpp)との共有メモリブリッジ
            // (実装ガイド §8.1)。index/pid は無い。
            DeviceSpec s;
            s.backend = DeviceSpec::Backend::Vasio;
            specs.push_back(s);
        } else if (a == L"--vasio-instance") {
            // gap 11: 直前の --vasio に instanceId を割り当てる(複数
            // インスタンス対応。既定は "0"、対応する DAW プロセス側は
            // 環境変数 SLUICE_VASIO_INSTANCE に同じ値を設定する必要が
            // ある、vasio/vasio_driver.cpp 参照)。
            const std::wstring instanceId = nextWStr(i, L"0");
            ++i;
            if (specs.empty() || specs.back().backend != DeviceSpec::Backend::Vasio) {
                fwprintf(stderr, L"--vasio-instance must directly follow --vasio\n");
                return 1;
            }
            specs.back().vasioInstanceId = instanceId;
        } else if (a == L"--vbcable-in" || a == L"--vbcable-out") {
            const bool wantIn = (a == L"--vbcable-in");
            auto eps = wasapi::DetectVbCable();
            const auto& opt = wantIn ? eps.virtualInput : eps.virtualOutput;
            if (!opt) {
                fwprintf(stderr, L"VB-CABLE %s endpoint not found (is it installed?)\n",
                        wantIn ? L"input" : L"output");
                return 1;
            }
            DeviceSpec s;
            s.backend = DeviceSpec::Backend::Wasapi;
            s.isCapture = wantIn;
            s.endpointId = opt->id;
            s.displayName = opt->name;
            s.backendLabel = "vbcable";
            specs.push_back(s);
        } else if (a == L"--vac-line") {
            const int line = nextInt(i, -1);
            ++i;
            auto lines = wasapi::DetectVac();
            auto it = std::find_if(lines.begin(), lines.end(),
                                   [&](const wasapi::VacLine& l) { return l.lineNumber == line; });
            if (it == lines.end()) {
                fwprintf(stderr, L"VAC line %d not found (is VAC installed?)\n", line);
                return 1;
            }
            if (it->capture) {
                DeviceSpec s;
                s.backend = DeviceSpec::Backend::Wasapi;
                s.isCapture = true;
                s.endpointId = it->capture->id;
                s.displayName = it->capture->name;
                s.backendLabel = "vac";
                specs.push_back(s);
            }
            if (it->render) {
                DeviceSpec s;
                s.backend = DeviceSpec::Backend::Wasapi;
                s.isCapture = false;
                s.endpointId = it->render->id;
                s.displayName = it->render->name;
                s.backendLabel = "vac";
                specs.push_back(s);
            }
        } else {
            fwprintf(stderr, L"unknown option: %s\n", a.c_str());
            return 1;
        }
    }

    LoadLatencyDb(latencyDbPath);

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        fwprintf(stderr, L"CoInitializeEx failed\n");
        return 1;
    }

    if (listOnly) {
        ListDevices();
        return 0;
    }

    if (specs.empty()) {
        wprintf(L"Usage: sluice-engine [--list]\n"
                L"  [--asio-in|--asio-out <idx>]... [--wasapi-in|--wasapi-out <idx>]...\n"
                L"  [--ks-in|--ks-out <idx>]... [--loopback-pid <pid>]...\n"
                L"  [--vbcable-in] [--vbcable-out] [--vac-line <n>] [--vasio]\n"
                L"  [--channels <n>] [--low-latency] [--auto-route]\n"
                L"Run with --list to see available device indices.\n");
        return 0;
    }

    // 実装ガイド §4.1.5: 同一 ASIO ドライバの二重オープンは大半のドライバで
    // 不可(シングルクライアント実装が多い)。方向によらず 1 回までに制限する。
    {
        std::vector<int> usedAsioIndices;
        for (const auto& s : specs) {
            if (s.backend != DeviceSpec::Backend::Asio) continue;
            if (std::find(usedAsioIndices.begin(), usedAsioIndices.end(), s.index) !=
                usedAsioIndices.end()) {
                fwprintf(stderr,
                        L"error: ASIO driver %d requested more than once (single-client "
                        L"assumption, guide §4.1.5)\n",
                        s.index);
                return 1;
            }
            usedAsioIndices.push_back(s.index);
        }
    }

    DeviceStreamConfig baseConfig;
    baseConfig.sampleRate = kSampleRate;
    baseConfig.channels = reqChannels;
    // 実装ガイド §5.2.3: 積極的低遅延モードはオプトイン。既定はデバイス既定
    // 周期で開き、他アプリを巻き込まない。
    baseConfig.aggressiveLowLatency = lowLatency;
    baseConfig.preferredBufferFrames = lowLatency ? 64 : 0;
    baseConfig.rawMode = lowLatency;

    std::wstring err;
    std::unique_ptr<MixerState> initial = OpenAndBuild(specs, baseConfig, autoRoute, &err);
    if (!initial) {
        fwprintf(stderr, L"failed to start: %s\n", err.c_str());
        return 1;
    }

    // 制御プレーンの状態はすべてこのミューテックス経由でのみ触る。監視ループ
    // (デバイスリセット検知・devices_changed 通知)と IPC スレッド
    // (pipe_server.h は登録した各ハンドラを 1 本の内部スレッドから順番に
    // 呼ぶ)の 2 つが g_state を共有するため必要(RT スレッドはこの一切に
    // 関与しない。GraphHandle 経由の RCU で完全に分離されている)。
    std::mutex controlMutex;
    std::unique_ptr<MixerState> state = std::move(initial);

    wprintf(L"started:\n");
    PrintStats(*state);

    ipc::PipeServer pipeServer(L"\\\\.\\pipe\\sluice-engine");
    // echo は既存 UI(ui/SluiceUi/MainWindow.xaml.cs)が使うため維持する。
    pipeServer.RegisterMethod("echo", [](const JsonValue& params) { return params; });

    pipeServer.RegisterMethod("get_devices", [&](const JsonValue&) {
        std::lock_guard<std::mutex> lock(controlMutex);
        return BuildDevicesJson(*state);
    });

    pipeServer.RegisterMethod("get_topology", [&](const JsonValue&) {
        std::lock_guard<std::mutex> lock(controlMutex);
        return BuildTopologyJson(*state);
    });

    pipeServer.RegisterMethod("set_param", [&](const JsonValue& params) -> JsonValue {
        std::lock_guard<std::mutex> lock(controlMutex);
        const std::string target = params.At("target").AsString();
        const int index = params.At("index").AsInt(-1);

        if (target == "strip") {
            if (index < 0 || (size_t)index >= state->strips.size())
                throw std::runtime_error("set_param: strip index out of range");
            StripParams& p = state->strips[(size_t)index].params;
            if (params.Has("gainDb")) p.gainDb = (float)params.At("gainDb").AsNumber(p.gainDb);
            if (params.Has("mute")) p.mute = params.At("mute").AsBool(p.mute);
            if (params.Has("solo")) p.solo = params.At("solo").AsBool(p.solo);
            if (params.Has("routingGain")) {
                const auto& items = params.At("routingGain").Items();
                for (size_t bi = 0; bi < items.size() && bi < StripParams::kMaxBuses; ++bi)
                    p.routingGain[bi] = (float)items[bi].AsNumber(p.routingGain[bi]);
            }
            if (params.Has("gate")) GateParamsFromJson(params.At("gate"), &p.gate);
            if (params.Has("eq")) EqParamsFromJson(params.At("eq"), &p.eq);
            if (params.Has("comp")) CompParamsFromJson(params.At("comp"), &p.comp);
            if (state->controlGraph && (size_t)index < state->controlGraph->StripCount())
                state->controlGraph->Strip((size_t)index).PublishParams(p);
            return JsonValue::MakeObject();
        }
        if (target == "bus") {
            if (index < 0 || (size_t)index >= state->buses.size())
                throw std::runtime_error("set_param: bus index out of range");
            BusParams& p = state->buses[(size_t)index].params;
            if (params.Has("gainDb")) p.gainDb = (float)params.At("gainDb").AsNumber(p.gainDb);
            if (params.Has("limiter")) LimiterParamsFromJson(params.At("limiter"), &p.limiter);
            if (state->controlGraph && (size_t)index < state->controlGraph->BusCount())
                state->controlGraph->Bus((size_t)index).PublishParams(p);
            return JsonValue::MakeObject();
        }
        throw std::runtime_error("set_param: target must be \"strip\" or \"bus\"");
    });

    pipeServer.RegisterMethod("add_strip", [&](const JsonValue& params) -> JsonValue {
        std::lock_guard<std::mutex> lock(controlMutex);
        const int boundaryIndex = params.At("boundaryIndex").AsInt(-1);
        const int channel = params.At("channel").AsInt(-1);
        if (boundaryIndex < 0 || (size_t)boundaryIndex >= state->boundaries.size())
            throw std::runtime_error("add_strip: boundaryIndex out of range");
        const int deviceIndex = state->boundaries[(size_t)boundaryIndex].deviceIndex;
        IAudioDevice* dev = state->devices[(size_t)deviceIndex].device.get();
        if (channel < 0 || !dev->CaptureRing(channel))
            throw std::runtime_error("add_strip: invalid channel for this boundary's device");

        StripSpec spec;
        spec.boundaryIndex = boundaryIndex;
        spec.deviceIndex = deviceIndex;
        spec.channel = channel;
        if (params.Has("gainDb")) spec.params.gainDb = (float)params.At("gainDb").AsNumber(0.0);
        state->strips.push_back(spec);
        RebuildEngineGraph(*state);

        JsonValue result = JsonValue::MakeObject();
        result["index"] = (int)(state->strips.size() - 1);
        return result;
    });

    pipeServer.RegisterMethod("remove_strip", [&](const JsonValue& params) -> JsonValue {
        std::lock_guard<std::mutex> lock(controlMutex);
        const int index = params.At("index").AsInt(-1);
        if (index < 0 || (size_t)index >= state->strips.size())
            throw std::runtime_error("remove_strip: index out of range");
        state->strips.erase(state->strips.begin() + index);
        RebuildEngineGraph(*state);
        return JsonValue::MakeObject();
    });

    if (!pipeServer.Start()) {
        wprintf(L"engine: failed to start control pipe \\\\.\\pipe\\sluice-engine "
                 L"(already in use by another process?)\n");
        return 1;
    }
    wprintf(L"running. Press Ctrl+C to stop.\n");

    // --- 監視ループ(1 秒ごとに統計・push 通知、デバイスリセット要求を処理) ---
    for (;;) {
        Sleep(1000);
        std::lock_guard<std::mutex> lock(controlMutex);

        // 実装ガイド §3.2: RT 区間アロケーション検出(Debug 構成のみ有効。
        // Release では rtguard::Violations() は常に 0)。
        if (const long long violations = rtguard::Violations(); violations != 0) {
            fwprintf(stderr,
                    L"** RT-thread allocation detected: %lld violation(s) since last check "
                    L"(guide §3.2/§9) **\n",
                    violations);
            rtguard::ResetViolations();
        }

        bool anyReset = false;
        for (auto& e : state->devices)
            if (e.device->Status().resetRequested) { anyReset = true; break; }

        if (anyReset) {
            wprintf(L"** device reset requested: rebuilding pipeline **\n");
            const int oldMaxBlockFrames = state->maxBlockFrames;
            StopAndCloseAll(*state);
            std::wstring rebuildErr;
            auto next = OpenAndBuild(specs, baseConfig, autoRoute, &rebuildErr);
            if (next) {
                state = std::move(next);
                wprintf(L"rebuilt:\n");
                PrintStats(*state);
                // 実装ガイド §8.1 手順6: マスタークロックのブロックサイズが
                // 変わった(≒ vasio ブリッジ経由の DAW から見えるバッファ
                // サイズが変わった)場合、接続中の vasio ブリッジに DAW 側
                // リセットを要求する(vasio.dll が次回 bufferSwitch の前に
                // kAsioResetRequest を送出する、vasio/vasio_driver.cpp 参照)。
                if (state->maxBlockFrames != oldMaxBlockFrames)
                    for (auto* bridge : state->vasioBridges) bridge->RequestDawReset();
            } else {
                fwprintf(stderr, L"rebuild failed: %s (will retry)\n", rebuildErr.c_str());
                state = std::make_unique<MixerState>();  // IPC が壊れず空を返せるように
            }
        }

        JsonValue event = JsonValue::MakeObject();
        event["event"] = std::string("devices_changed");
        event["data"] = BuildDevicesJson(*state);
        pipeServer.Notify(event);
    }
    // ここには到達しない(Ctrl+C で終了)
}
