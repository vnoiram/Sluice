// main.cpp : ASIO A(入力) → CaptureRing(チャンネルごと) → ASRC → ASIO B(出力)
//            RenderRing(チャンネルごと) パススルー
//
// データフロー(実装ガイド §4.4 / §5.1 の IAudioDevice 契約に準拠):
//   A: OnBufferSwitch が各チャンネルの CaptureRing へ書く(デバイス内部で完結)
//   B: OnBufferSwitch が RenderRing を読み出す「前」に blockCallback が発火
//      する(実装ガイド §5.4.1 の「マスターコールバック」に相当)。
//      この blockCallback の中で、このファイル(エンジン)が A の
//      CaptureRing から ASRC 経由で 1 ブロック分読み出し、B の
//      RenderRing へ書き込む。ASRC/ドリフト補正はデバイス側ではなく
//      ここ(エンジン境界)に置く(実装ガイド §5.1)。
//   B がマスタークロック。ドリフト補正(PI)は B の blockCallback 内で回す。
//
// スレッド構成:
//   main スレッド  : COM(STA), デバイス管理, 1 秒ごとの統計表示, リセット監視
//   A の RT スレッド: ドライバ A が作る(bufferSwitch)。CaptureRing へ書くのみ。
//   B の RT スレッド: ドライバ B が作る(bufferSwitch)。blockCallback(エンジン
//                    の ASRC+ドリフト処理)→ RenderRing 読み出し、の順で走る。

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "crash/crash_handler.h"
#include "device/asio_host.h"
#include "dsp/drift.h"
#include "ipc/device_report.h"
#include "ipc/pipe_server.h"
#include "rt/spsc_ring.h"
#include "version.h"

using namespace asiohost;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int    kChannels   = 2;

struct Stats {
    std::atomic<uint64_t> outUnderrun{0};   // ASRC(エンジン境界)でのアンダーラン
};

struct Pipeline {
    std::vector<std::unique_ptr<AsrcReader>> asrc;   // チャンネルごと
    DriftController drift;
    Ema fillEma{0.99};
    std::atomic<double> ratioForUi{1.0};
    std::atomic<double> fillForUi{0.5};
    std::atomic<bool>   prefilled{false};
    Stats stats;
    std::vector<float> scratch;   // B 側エンジン処理用(起動前確保、1ch 分)
};

void PrintDrivers(const std::vector<DriverInfo>& list) {
    for (size_t i = 0; i < list.size(); ++i)
        wprintf(L"  [%zu] %s\n", i, list[i].name.c_str());
}

// engine/ipc/device_report.h は Windows API 非依存を保つため UTF-8
// std::string のみを扱う(オフライン単体テストを可能にするため)。
// wstring→UTF-8 変換はこの Windows 専用ファイル側の責務にする。
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int len =
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), len, nullptr, nullptr);
    return out;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    // 起動直後、他の何より先にクラッシュダンプ収集を有効化する
    // (実装ガイド §5.7)。
    crashhandler::Install();
    wprintf(L"sluice-engine %s\n", version::kStringW);

    // --- 引数処理 ---------------------------------------------------------
    int inIdx = -1, outIdx = -1;
    bool listOnly = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--list") listOnly = true;
        else if (a == L"--in"  && i + 1 < argc) inIdx  = _wtoi(argv[++i]);
        else if (a == L"--out" && i + 1 < argc) outIdx = _wtoi(argv[++i]);
    }

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        fwprintf(stderr, L"CoInitializeEx failed\n");
        return 1;
    }

    auto drivers = EnumerateDrivers();
    if (drivers.empty()) {
        fwprintf(stderr, L"No ASIO drivers found (HKLM\\SOFTWARE\\ASIO)\n");
        return 1;
    }
    if (listOnly || inIdx < 0 || outIdx < 0) {
        wprintf(L"ASIO drivers:\n");
        PrintDrivers(drivers);
        wprintf(L"\nUsage: sluice-engine --in <index> --out <index>\n");
        return 0;
    }
    if (inIdx == outIdx) {
        fwprintf(stderr, L"--in and --out must be different drivers "
                         L"(single-client assumption)\n");
        return 1;
    }

    DeviceStreamConfig cfg;
    cfg.sampleRate = kSampleRate;
    cfg.channels = kChannels;

    // --- デバイスオープン(リセット時にもここから再実行する)-------------
retry_open:
    AsioDevice devIn(drivers[(size_t)inIdx], /*isInput=*/true);
    AsioDevice devOut(drivers[(size_t)outIdx], /*isInput=*/false);
    std::wstring err;
    if (!devIn.Open(cfg, &err)) {
        fwprintf(stderr, L"open input failed: %s\n", err.c_str());
        return 1;
    }
    if (!devOut.Open(cfg, &err)) {
        fwprintf(stderr, L"open output failed: %s\n", err.c_str());
        return 1;
    }

    const long outBuf = devOut.BufferSize();
    wprintf(L"in : %s  (buffer %ld)\n", drivers[(size_t)inIdx].name.c_str(),
            devIn.BufferSize());
    wprintf(L"out: %s  (buffer %ld)\n", drivers[(size_t)outIdx].name.c_str(),
            outBuf);

    // --- パイプライン構築(RT 開始前に全メモリ確保)-----------------------
    Pipeline p;
    p.asrc.reserve(kChannels);
    for (int c = 0; c < kChannels; ++c)
        // A/B とも ASIO(常に RT Lane、実装ガイド §4.1.4)なので
        // SRC_SINC_FASTEST を使う(実装ガイド §4.3.2)。
        p.asrc.push_back(std::make_unique<AsrcReader>(*devIn.CaptureRing(c), outBuf, Lane::RT));
    p.scratch.resize((size_t)outBuf);

    // --- B(出力)側 = マスタークロック。RenderRing 読み出し直前に発火する
    //     blockCallback の中でエンジン処理(ASRC+ドリフト補正)を行う -------
    devOut.SetBlockCallback([&](int frames) {
        // プリフィル: A 側リングが半分たまるまで無音を出す(全チャンネル
        // 同時に書かれるため、代表してチャンネル 0 の充填率を見ればよい)
        if (!p.prefilled.load(std::memory_order_relaxed)) {
            if (devIn.CaptureRing(0)->FillRatio() < 0.5) {
                std::fill(p.scratch.begin(), p.scratch.begin() + frames, 0.0f);
                for (int c = 0; c < kChannels; ++c)
                    devOut.RenderRing(c)->Write(p.scratch.data(), (size_t)frames);
                return;
            }
            p.prefilled.store(true, std::memory_order_relaxed);
        }
        // 充填率 → 平滑化 → PI → リサンプル比(全チャンネル共通)
        const double fill = p.fillEma.Push(devIn.CaptureRing(0)->FillRatio());
        const double driftRatio = p.drift.Update(fill);
        const double srcRatio = 1.0 / driftRatio;   // libsamplerate は 出力/入力
        p.fillForUi.store(fill, std::memory_order_relaxed);
        p.ratioForUi.store(driftRatio, std::memory_order_relaxed);

        bool anyUnderrun = false;
        for (int c = 0; c < kChannels; ++c) {
            if (p.asrc[(size_t)c]->Read(p.scratch.data(), frames, srcRatio))
                anyUnderrun = true;
            devOut.RenderRing(c)->Write(p.scratch.data(), (size_t)frames);
        }
        if (anyUnderrun) {
            p.stats.outUnderrun.fetch_add(1, std::memory_order_relaxed);
            p.prefilled.store(false, std::memory_order_relaxed); // 再プリフィル
        }
    });

    // --- IPC(名前付きパイプ、実装ガイド §5.6)------------------------------
    // main.cpp の Phase-0 デバイス(devIn/devOut)に PipeServer を配線する。
    // JSON スキーマはデバイスの配列にしておき(固定 A/B ペア決め打ちに
    // しない)、将来 EngineGraph に統合されても破壊的変更にならないように
    // する。UI に必ず出すべきもの(実装ガイド §5.6): レーン・実効
    // レイテンシ・xrun カウンタ・ASRC の現在比。
    auto buildDevicesArray = [&]() {
        std::vector<JsonValue> reports;
        const double ratio = p.ratioForUi.load(std::memory_order_relaxed);
        const std::string inName = WideToUtf8(drivers[(size_t)inIdx].name);
        const std::string outName = WideToUtf8(drivers[(size_t)outIdx].name);
        reports.push_back(ipc::DeviceReportToJson("asio:in:" + inName, inName, "asio",
                                                  /*isCapture=*/true, devIn.Status(),
                                                  devIn.Probe(kSampleRate), ratio));
        reports.push_back(ipc::DeviceReportToJson("asio:out:" + outName, outName, "asio",
                                                  /*isCapture=*/false, devOut.Status(),
                                                  devOut.Probe(kSampleRate), ratio));
        return ipc::DeviceReportsToJsonArray(reports);
    };

    ipc::PipeServer pipeServer(L"\\\\.\\pipe\\sluice-engine");
    // echo は既存 UI(ui/SluiceUi/MainWindow.xaml.cs)が使うため維持する。
    pipeServer.RegisterMethod("echo", [](const JsonValue& params) { return params; });
    pipeServer.RegisterMethod("get_devices",
                              [&](const JsonValue&) { return buildDevicesArray(); });
    pipeServer.Start();

    // --- 開始 --------------------------------------------------------------
    devIn.Start();
    devOut.Start();
    wprintf(L"running. Press Ctrl+C to stop.\n");

    // --- 監視ループ(1 秒ごとに統計・push 通知、リセット要求を処理)---------
    for (;;) {
        Sleep(1000);
        const DeviceStatus inStatus = devIn.Status();
        const DeviceStatus outStatus = devOut.Status();
        wprintf(L"fill=%5.1f%%  ratio=%.7f  xrun(in=%llu out=%llu)  "
                L"cb(A=%llu B=%llu)\n",
                p.fillForUi.load() * 100.0, p.ratioForUi.load(),
                (unsigned long long)inStatus.overrunCount,
                (unsigned long long)p.stats.outUnderrun.load(),
                (unsigned long long)inStatus.callbackCount,
                (unsigned long long)outStatus.callbackCount);

        // push 通知(実装ガイド §5.6)。PipeServer::Notify() はロック+ヒープ
        // 確保を伴うため、RT スレッドではなくこの監視ループ(制御スレッド)
        // から呼ぶ。
        JsonValue event = JsonValue::MakeObject();
        event["event"] = std::string("devices_changed");
        event["data"] = buildDevicesArray();
        pipeServer.Notify(event);

        if (inStatus.resetRequested || outStatus.resetRequested) {
            wprintf(L"** kAsioResetRequest: rebuilding devices **\n");
            pipeServer.Stop();
            devIn.Close();
            devOut.Close();
            goto retry_open;   // PoC の最単純対応: 全体作り直し
        }
    }
    // ここには到達しない(Ctrl+C で終了)
}
