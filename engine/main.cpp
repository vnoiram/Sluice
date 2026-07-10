// main.cpp : ASIO A(入力) → リング → ASRC → ASIO B(出力) パススルー
//
// データフロー(実装ガイド §4.4):
//   A.bufferSwitch: 入力2ch → float 変換 → ring.Write
//   B.bufferSwitch: ring から ASRC 経由で bufferSize フレーム取得 → 出力
//   B がマスタークロック。ドリフト補正(PI)は B 側で回す。
//
// スレッド構成:
//   main スレッド  : COM(STA), デバイス管理, 1 秒ごとの統計表示, リセット監視
//   A の RT スレッド: ドライバ A が作る(bufferSwitch)
//   B の RT スレッド: ドライバ B が作る(bufferSwitch)

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>

#include "device/asio_host.h"
#include "dsp/drift.h"
#include "rt/spsc_ring.h"

using namespace asiohost;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int    kChannels   = 2;

struct Stats {
    std::atomic<uint64_t> inOverrun{0};
    std::atomic<uint64_t> outUnderrun{0};
};

struct Pipeline {
    std::unique_ptr<SpscRing<float>> ring;   // インターリーブ float
    std::unique_ptr<AsrcReader> asrc;
    DriftController drift;
    Ema fillEma{0.99};
    std::atomic<double> ratioForUi{1.0};
    std::atomic<double> fillForUi{0.5};
    std::atomic<bool>   prefilled{false};
    Stats stats;
    std::vector<float> inScratch;    // A 側 RT 用(起動前確保)
    std::vector<float> outScratch;   // B 側 RT 用(起動前確保)
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
        wprintf(L"\nUsage: poc --in <index> --out <index>\n");
        return 0;
    }
    if (inIdx == outIdx) {
        fwprintf(stderr, L"--in and --out must be different drivers "
                         L"(single-client assumption)\n");
        return 1;
    }

    // --- デバイスオープン(リセット時にもここから再実行する)-------------
retry_open:
    AsioDevice devIn, devOut;
    std::wstring err;
    if (!devIn.Open(drivers[inIdx], kSampleRate, /*useInput=*/true, &err)) {
        fwprintf(stderr, L"open input failed: %s\n", err.c_str());
        return 1;
    }
    if (!devOut.Open(drivers[outIdx], kSampleRate, /*useInput=*/false, &err)) {
        fwprintf(stderr, L"open output failed: %s\n", err.c_str());
        return 1;
    }

    const long inBuf = devIn.BufferSize();
    const long outBuf = devOut.BufferSize();
    wprintf(L"in : %s  (buffer %ld)\n", drivers[inIdx].name.c_str(), inBuf);
    wprintf(L"out: %s  (buffer %ld)\n", drivers[outIdx].name.c_str(), outBuf);

    // --- パイプライン構築(RT 開始前に全メモリ確保)-----------------------
    Pipeline p;
    // リング容量: 大きい方のバッファ×16 フレーム分を 2 の冪へ切り上げ
    size_t frames = (size_t)std::max(inBuf, outBuf) * 16;
    size_t cap = 1; while (cap < frames * kChannels) cap <<= 1;
    p.ring = std::make_unique<SpscRing<float>>(cap);
    p.asrc = std::make_unique<AsrcReader>(*p.ring, kChannels, outBuf);
    p.inScratch.resize((size_t)inBuf * kChannels);
    p.outScratch.resize((size_t)outBuf * kChannels);

    // --- A(入力)側 RT ---------------------------------------------------
    devIn.SetProcessCallback([&](long index) {
        devIn.ConvertInToFloat(index, p.inScratch.data());
        const size_t n = p.inScratch.size();
        if (p.ring->Write(p.inScratch.data(), n) < n)
            p.stats.inOverrun.fetch_add(1, std::memory_order_relaxed);
    });

    // --- B(出力)側 RT = マスタークロック --------------------------------
    devOut.SetProcessCallback([&](long index) {
        // プリフィル: リングが半分たまるまで無音を出す
        if (!p.prefilled.load(std::memory_order_relaxed)) {
            if (p.ring->FillRatio() < 0.5) {
                std::fill(p.outScratch.begin(), p.outScratch.end(), 0.0f);
                devOut.ConvertFloatToOut(index, p.outScratch.data());
                return;
            }
            p.prefilled.store(true, std::memory_order_relaxed);
        }
        // 充填率 → 平滑化 → PI → リサンプル比
        const double fill = p.fillEma.Push(p.ring->FillRatio());
        const double driftRatio = p.drift.Update(fill);
        const double srcRatio = 1.0 / driftRatio;   // libsamplerate は 出力/入力
        p.fillForUi.store(fill, std::memory_order_relaxed);
        p.ratioForUi.store(driftRatio, std::memory_order_relaxed);

        if (p.asrc->Read(p.outScratch.data(), outBuf, srcRatio)) {
            p.stats.outUnderrun.fetch_add(1, std::memory_order_relaxed);
            p.prefilled.store(false, std::memory_order_relaxed); // 再プリフィル
        }
        devOut.ConvertFloatToOut(index, p.outScratch.data());
    });

    // --- 開始 --------------------------------------------------------------
    devIn.Start();
    devOut.Start();
    wprintf(L"running. Press Ctrl+C to stop.\n");

    // --- 監視ループ(1 秒ごとに統計、リセット要求を処理)-------------------
    for (;;) {
        Sleep(1000);
        wprintf(L"fill=%5.1f%%  ratio=%.7f  xrun(in=%llu out=%llu)  "
                L"cb(A=%llu B=%llu)\n",
                p.fillForUi.load() * 100.0, p.ratioForUi.load(),
                (unsigned long long)p.stats.inOverrun.load(),
                (unsigned long long)p.stats.outUnderrun.load(),
                (unsigned long long)devIn.CallbackCount(),
                (unsigned long long)devOut.CallbackCount());

        if (devIn.ResetRequested() || devOut.ResetRequested()) {
            wprintf(L"** kAsioResetRequest: rebuilding devices **\n");
            devIn.Close();
            devOut.Close();
            goto retry_open;   // PoC の最単純対応: 全体作り直し
        }
    }
    // ここには到達しない(Ctrl+C で終了)
}
