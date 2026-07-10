// test_engine_graph.cpp : N×M ルーティング/ミックスと RCU グラフ差し替えの
// オフライン回帰テスト(実装ガイド §5.4)。
//
// 実オーディオデバイス不要。定数信号を入力リングに流し込み、
// StripRuntime → BusRuntime の N×M ミックスが期待通りの値になることを
// 確認する(テスト1)。GraphHandle(RCU)の安全性は、RT 役のスレッドが
// Process() を回し続ける横で制御スレッドが大量に Publish() し続けても
// クラッシュ/ハングしないことを確認するストレステストで検証する
// (テスト2)。

#include "graph/engine_graph.h"
#include "graph/gain_util.h"
#include "rt/spsc_ring.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

// ===========================================================================
// RT 区間アロケーション検出フック(他の test_*.cpp と同じ仕組み)
// ===========================================================================
namespace {
thread_local bool g_rtRegion = false;
std::atomic<long long> g_rtAllocViolations{0};
}  // namespace

void* operator new(std::size_t sz) {
    if (g_rtRegion) g_rtAllocViolations.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(sz);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

struct RtGuard {
    RtGuard() { g_rtRegion = true; }
    ~RtGuard() { g_rtRegion = false; }
};

void Fail(const std::string& what) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    std::exit(1);
}

void CheckClose(double actual, double expected, double tol, const std::string& label) {
    if (std::fabs(actual - expected) > tol) {
        Fail(label + ": expected " + std::to_string(expected) + " got " +
             std::to_string(actual) + " (tol " + std::to_string(tol) + ")");
    }
}

constexpr int kBlockFrames = 256;
constexpr double kSampleRate = 48000.0;
constexpr size_t kRingCap = 8192;  // 2 の冪。kBlockFrames*16 程度以上

// リングを一定値の信号で埋めるヘルパ(テスト用の疑似デバイス)。
void FillConstant(SpscRing<float>& ring, float value, size_t frames) {
    std::vector<float> buf(frames, value);
    ring.Write(buf.data(), frames);
}

// ---------------------------------------------------------------------------
// テスト 1: N×M ミックス(送り/ミュート/サミング/バスゲイン)の検算
// ---------------------------------------------------------------------------
void TestMixMath() {
    // 検算をシンプルにするため、dB は 0(unity, linear=1.0)か 20(linear=10.0)
    // という「割り切れる」値だけを使う(ASRC 由来の小さな誤差だけを
    // 許容すればよくなる)。
    if (std::fabs(DbToLinear(0.0f) - 1.0f) > 1e-6f) Fail("DbToLinear(0) sanity");
    if (std::fabs(DbToLinear(20.0f) - 10.0f) > 1e-4f) Fail("DbToLinear(20) sanity");

    auto ring0 = std::make_unique<SpscRing<float>>(kRingCap);
    auto ring1 = std::make_unique<SpscRing<float>>(kRingCap);
    auto ring2 = std::make_unique<SpscRing<float>>(kRingCap);

    FillConstant(*ring0, 1.0f, kRingCap / 2 + (size_t)kBlockFrames * 4);
    FillConstant(*ring1, 2.0f, kRingCap / 2 + (size_t)kBlockFrames * 4);
    FillConstant(*ring2, 4.0f, kRingCap / 2 + (size_t)kBlockFrames * 4);

    std::vector<InputBoundary> boundaries;
    boundaries.emplace_back(*ring0);
    boundaries.emplace_back(*ring1);
    boundaries.emplace_back(*ring2);

    StripParams p0;
    p0.routingGain[0] = 0.0f;  // bus0 へ unity
    p0.routingGain[2] = 0.0f;  // bus2 へも unity(bus1 は既定で muted)
    StripParams p1;
    p1.routingGain[1] = 0.0f;  // bus1 へ unity
    p1.routingGain[2] = 0.0f;  // bus2 へも unity(bus0 は既定で muted)
    StripParams p2;
    p2.routingGain[0] = 0.0f;  // bus0/1/2 すべてへ unity
    p2.routingGain[1] = 0.0f;
    p2.routingGain[2] = 0.0f;

    std::vector<StripRuntime> strips;
    strips.emplace_back(*ring0, kBlockFrames, kSampleRate, p0, /*boundaryIndex=*/0);
    strips.emplace_back(*ring1, kBlockFrames, kSampleRate, p1, /*boundaryIndex=*/1);
    strips.emplace_back(*ring2, kBlockFrames, kSampleRate, p2, /*boundaryIndex=*/2);

    auto out0 = std::make_unique<SpscRing<float>>(kRingCap);
    auto out1 = std::make_unique<SpscRing<float>>(kRingCap);
    auto out2 = std::make_unique<SpscRing<float>>(kRingCap);

    // リミッタは既定で有効(実装ガイド §5.5)だが、このテストはミックス/
    // ルーティング算術の検算が目的で、bus2 の期待値(70.0)はリミッタの
    // 出力上限を大きく超えるため、ここでは無効化して切り分ける
    // (リミッタ自体の検証は別途行うのが筋)。
    BusParams b0;  // 0dB
    b0.limiter.enabled = false;
    BusParams b1;  // 0dB
    b1.limiter.enabled = false;
    BusParams b2;
    b2.gainDb = 20.0f;  // 10 倍
    b2.limiter.enabled = false;

    std::vector<BusRuntime> buses;
    buses.emplace_back(kBlockFrames, std::vector<SpscRing<float>*>{out0.get()}, b0);
    buses.emplace_back(kBlockFrames, std::vector<SpscRing<float>*>{out1.get()}, b1);
    buses.emplace_back(kBlockFrames, std::vector<SpscRing<float>*>{out2.get()}, b2);

    EngineGraph graph(std::move(boundaries), std::move(strips), std::move(buses));

    constexpr int kWarmupBlocks = 64;  // ASRC/プリフィルが定常状態に収束するまで
    constexpr int kCheckBlocks = 8;
    std::vector<float> readBuf((size_t)kBlockFrames);

    for (int i = 0; i < kWarmupBlocks; ++i) {
        graph.Process(kBlockFrames);
        out0->Read(readBuf.data(), (size_t)kBlockFrames);
        out1->Read(readBuf.data(), (size_t)kBlockFrames);
        out2->Read(readBuf.data(), (size_t)kBlockFrames);
        FillConstant(*ring0, 1.0f, (size_t)kBlockFrames);
        FillConstant(*ring1, 2.0f, (size_t)kBlockFrames);
        FillConstant(*ring2, 4.0f, (size_t)kBlockFrames);
    }

    double sum0 = 0, sum1 = 0, sum2 = 0;
    long long n0 = 0, n1 = 0, n2 = 0;
    for (int i = 0; i < kCheckBlocks; ++i) {
        {
            RtGuard g;
            graph.Process(kBlockFrames);
        }
        size_t g0 = out0->Read(readBuf.data(), (size_t)kBlockFrames);
        for (size_t j = 0; j < g0; ++j) { sum0 += readBuf[j]; ++n0; }
        size_t g1 = out1->Read(readBuf.data(), (size_t)kBlockFrames);
        for (size_t j = 0; j < g1; ++j) { sum1 += readBuf[j]; ++n1; }
        size_t g2 = out2->Read(readBuf.data(), (size_t)kBlockFrames);
        for (size_t j = 0; j < g2; ++j) { sum2 += readBuf[j]; ++n2; }
        FillConstant(*ring0, 1.0f, (size_t)kBlockFrames);
        FillConstant(*ring1, 2.0f, (size_t)kBlockFrames);
        FillConstant(*ring2, 4.0f, (size_t)kBlockFrames);
    }

    if (g_rtAllocViolations.load() != 0)
        Fail("mix math: allocation occurred inside EngineGraph::Process()");
    if (n0 == 0 || n1 == 0 || n2 == 0) Fail("no samples collected from output buses");

    const double avg0 = sum0 / (double)n0;  // strip0(1.0) + strip2(4.0)
    const double avg1 = sum1 / (double)n1;  // strip1(2.0) + strip2(4.0)
    const double avg2 = sum2 / (double)n2;  // (1.0+2.0+4.0) * 10

    CheckClose(avg0, 5.0, 0.1, "bus0 (strip0+strip2)");
    CheckClose(avg1, 6.0, 0.1, "bus1 (strip1+strip2)");
    CheckClose(avg2, 70.0, 1.0, "bus2 (all strips, x10 bus gain)");

    std::printf("PASS: mix math  bus0=%.4f(exp 5.0)  bus1=%.4f(exp 6.0)  "
                "bus2=%.4f(exp 70.0)\n",
                avg0, avg1, avg2);
}

// ---------------------------------------------------------------------------
// テスト 2: GraphHandle(RCU)の差し替えストレステスト
// ---------------------------------------------------------------------------
void TestRcuStress() {
    constexpr int kPublishCount = 300;
    constexpr int kWatchdogSeconds = 60;

    auto ring = std::make_unique<SpscRing<float>>(kRingCap);
    auto out = std::make_unique<SpscRing<float>>(kRingCap);
    FillConstant(*ring, 1.0f, kRingCap / 2 + (size_t)kBlockFrames * 4);

    GraphHandle handle;
    auto makeGraph = [&](float gainDb) {
        std::vector<InputBoundary> boundaries;
        boundaries.emplace_back(*ring);
        StripParams p;
        p.routingGain[0] = 0.0f;
        p.gainDb = gainDb;
        std::vector<StripRuntime> strips;
        strips.emplace_back(*ring, kBlockFrames, kSampleRate, p, /*boundaryIndex=*/0);
        std::vector<BusRuntime> buses;
        buses.emplace_back(kBlockFrames, std::vector<SpscRing<float>*>{out.get()}, BusParams{});
        return std::make_unique<EngineGraph>(std::move(boundaries), std::move(strips),
                                             std::move(buses));
    };

    handle.Publish(makeGraph(0.0f));

    std::atomic<bool> stop{false};
    std::atomic<bool> done{false};
    std::atomic<long long> blocksProcessed{0};

    std::thread watchdog([&] {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(kWatchdogSeconds);
        while (!done.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() > deadline) {
                std::fprintf(stderr,
                             "FAIL: RCU stress watchdog timeout (%ds) "
                             "(deadlock/livelock suspected)\n",
                             kWatchdogSeconds);
                std::abort();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    });

    std::thread rtThread([&] {
        std::vector<float> scratch((size_t)kBlockFrames);
        while (!stop.load(std::memory_order_relaxed)) {
            EngineGraph* graph;
            {
                // 検証対象は Acquire()/Process()(実際の RT パス)のみ。
                // FillConstant はこのテストが疑似デバイスに給餌するための
                // scaffolding で、内部で一時 vector を確保する
                // (real な AsioDevice/WasapiDevice の RT コールバックは
                // 事前確保バッファに読み書きするだけで、これには相当しない)。
                RtGuard g;
                graph = handle.Acquire();
                if (graph) graph->Process(kBlockFrames);
            }
            if (graph) {
                out->Read(scratch.data(), (size_t)kBlockFrames);
                FillConstant(*ring, 1.0f, (size_t)kBlockFrames);
                blocksProcessed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    for (int i = 0; i < kPublishCount; ++i) {
        handle.Publish(makeGraph((float)(i % 10)));
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    stop.store(true, std::memory_order_relaxed);
    rtThread.join();
    done.store(true, std::memory_order_release);
    watchdog.join();

    if (g_rtAllocViolations.load() != 0)
        Fail("RCU stress: allocation occurred inside RT-guarded region");
    if (blocksProcessed.load() == 0)
        Fail("RCU stress: RT thread never processed a block");

    std::printf("PASS: RCU stress (%d publishes, %lld blocks processed, no crash)\n",
                kPublishCount, (long long)blocksProcessed.load());
}

}  // namespace

int main() {
    TestMixMath();
    g_rtAllocViolations.store(0);
    TestRcuStress();
    std::printf("ALL PASS: engine_graph (mix math + RCU stress)\n");
    return 0;
}
