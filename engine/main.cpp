// main.cpp : ASIO A(入力) → CaptureRing(チャンネルごと) → ASRC → ASIO B(出力)
//            RenderRing(チャンネルごと) パススルー
//
// データフロー(実装ガイド §4.4 / §5.1 の IAudioDevice 契約に準拠):
//   A: OnBufferSwitch が各チャンネルの CaptureRing へ書く(デバイス内部で完結)
//   B: OnBufferSwitch が RenderRing を読み出す「前」に blockCallback が発火
//      する(実装ガイド §5.4.2 の「マスターコールバック」に相当)。
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

#include "device/asio_host.h"
#include "dsp/drift.h"
#include "rt/spsc_ring.h"

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

} // namespace

int wmain(int argc, wchar_t** argv) {
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
        p.asrc.push_back(std::make_unique<AsrcReader>(*devIn.CaptureRing(c), outBuf));
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

    // --- 開始 --------------------------------------------------------------
    devIn.Start();
    devOut.Start();
    wprintf(L"running. Press Ctrl+C to stop.\n");

    // --- 監視ループ(1 秒ごとに統計、リセット要求を処理)-------------------
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

        if (inStatus.resetRequested || outStatus.resetRequested) {
            wprintf(L"** kAsioResetRequest: rebuilding devices **\n");
            devIn.Close();
            devOut.Close();
            goto retry_open;   // PoC の最単純対応: 全体作り直し
        }
    }
    // ここには到達しない(Ctrl+C で終了)
}
