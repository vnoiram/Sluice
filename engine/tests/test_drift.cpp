// test_drift.cpp : モックデバイス(FakeDevice)によるドリフト補正の回帰テスト
//
// 実装ガイド §8:
//   「モックデバイス: FakeDevice(rate=48000*(1+drift), jitter=...) を作り、
//    実機なしで CI 上でドリフト補正を回帰テストできるようにする。
//    これが品質の生命線」
//
// ここでは main.cpp の B(出力/マスタークロック)側ロジックをそのまま模倣し、
// A(入力)側だけを「実クロックが ppm 単位でずれた仮想デバイス」に置き換えて
// 大量の論理ブロックを(実時間を待たずに)流し込むことで、実時間 30 分相当の
// ソークテストを数秒のCPU時間に圧縮して回す。
//
// 注意: これは drift.h(PI コントローラ + ASRC)の収束特性だけを検証する。
// README 記載の合格基準のうち「サイン波ループバックの波形不連続チェック」
// (実装ガイド §4.4 の 3)は実オーディオ経路が要るため、この自動テストの
// スコープ外(実機での手動検証が必要)。

#include "dsp/drift.h"
#include "rt/spsc_ring.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>
#include <string>
#include <vector>

// ===========================================================================
// RT 区間アロケーション検出フック(test_spsc_ring.cpp と同じ仕組み)
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

// ---------------------------------------------------------------------------
// FakeDevice: 実装ガイド §8 の想定そのもの。
// 「マスター(出力側)の1ブロック分の経過時間」を渡されるたびに、
// 自分の実クロック(48000*(1+driftPpm/1e6))で何フレーム分溜まったかを
// 積算し、ブロック境界を跨いだら writeBlock を呼ぶ。
//
// jitter: コールバック配送タイミングのゆらぎを模擬する。フレーム総量の
// 期待値(=長期平均レート)を変えないよう、"送出を確率的に1ブロック分だけ
// 遅延させてまとめて出す" 形でバースト性を注入する(ドリフト値そのものを
// 汚染しない)。
// ---------------------------------------------------------------------------
class FakeProducer {
public:
    FakeProducer(double driftPpm, int blockFrames, double jitterHoldbackProb,
                 uint32_t seed)
        : actualHz_(48000.0 * (1.0 + driftPpm / 1.0e6)),
          blockFrames_(blockFrames),
          holdbackProb_(jitterHoldbackProb),
          rng_(seed) {}

    template <typename WriteFn>
    void Advance(double dtSeconds, WriteFn&& writeBlock) {
        framesDue_ += dtSeconds * actualHz_;
        // 2 ブロック分以上溜まったら強制的に吐き出す(無限の holdback を防ぐ)
        while (framesDue_ >= blockFrames_) {
            if (framesDue_ < 2.0 * blockFrames_ &&
                uni_(rng_) < holdbackProb_) {
                break;  // このイテレーションは見送り(次回まとめて出る)
            }
            writeBlock(blockFrames_);
            framesDue_ -= blockFrames_;
        }
    }

private:
    double actualHz_;
    int blockFrames_;
    double holdbackProb_;
    std::mt19937 rng_;
    std::uniform_real_distribution<double> uni_{0.0, 1.0};
    double framesDue_ = 0.0;
};

struct Scenario {
    double driftPpm;
    bool expectZeroXrunAfterWarmup;  // ±maxCorr(500ppm) 内なら true
};

// ---------------------------------------------------------------------------
// 1 シナリオを実行し、収束後の統計を検証する
// ---------------------------------------------------------------------------
void RunScenario(const Scenario& sc) {
    constexpr int kChannels = 2;
    constexpr int kBlockFrames = 256;      // ASIO の典型的な preferred buffer
    constexpr double kSimSeconds = 1800.0; // 実時間30分相当を圧縮してシミュレート
    // ±400ppm 級(補正上限 500ppm に近い)の積分項ワインドアップは
    // 既定ゲイン(kp=1e-4, ki=1e-7)だと "数十秒" では収束しきらないため、
    // 収束後統計を取り始めるまでに十分な余裕を持たせる。
    constexpr double kWarmupSeconds = 600.0;

    const size_t frames = (size_t)kBlockFrames * 16;
    size_t cap = 1;
    while (cap < frames * kChannels) cap <<= 1;

    SpscRing<float> ring(cap);
    AsrcReader asrc(ring, kChannels, kBlockFrames);
    DriftController drift;
    Ema fillEma(0.99);

    FakeProducer producer(sc.driftPpm, kBlockFrames, /*jitterHoldbackProb=*/0.1,
                          /*seed=*/(uint32_t)(1000 + sc.driftPpm));

    std::vector<float> inScratch((size_t)kBlockFrames * kChannels, 0.0f);
    std::vector<float> outScratch((size_t)kBlockFrames * kChannels, 0.0f);

    const double dt = (double)kBlockFrames / 48000.0;  // マスター(出力)の1ブロック時間
    const long long totalIters = (long long)(kSimSeconds / dt);
    const long long warmupIters = (long long)(kWarmupSeconds / dt);

    bool prefilled = false;
    long long xrunAfterWarmup = 0;
    long long inOverrunAfterWarmup = 0;
    long long firstOverrunIter = -1;
    double fillMinTail = 1.0, fillMaxTail = 0.0, fillSumTail = 0.0;
    long long tailSamples = 0;
    double lastRatio = 1.0;

    for (long long it = 0; it < totalIters; ++it) {
        // --- A(入力)側 RT 相当: ドリフトしたクロックでリングへ書く ---
        {
            RtGuard g;
            producer.Advance(dt, [&](int n) {
                size_t nFloats = (size_t)n * kChannels;
                // 起動直後の初期収束(プリフィル/最初の PI 整定)中の
                // オーバーランは許容する。プリフィル自体が「消費側が
                // わざと読み出さずに溜める」フェーズなので、ここでの
                // 一時的な溢れは実装ガイド §4.4 の合格基準(収束後 xrun=0)
                // の対象外。warmupIters 以降だけを厳密にチェックする。
                if (ring.Write(inScratch.data(), nFloats) < nFloats) {
                    if (firstOverrunIter < 0) firstOverrunIter = it;
                    if (it >= warmupIters) ++inOverrunAfterWarmup;
                }
            });
        }

        // --- B(出力)側 RT 相当: main.cpp と同じロジック ---
        RtGuard g;
        if (!prefilled) {
            if (ring.FillRatio() < 0.5) continue;
            prefilled = true;
        }
        const double fill = fillEma.Push(ring.FillRatio());
        const double driftRatio = drift.Update(fill);
        const double srcRatio = 1.0 / driftRatio;
        lastRatio = driftRatio;

        bool underrun = asrc.Read(outScratch.data(), kBlockFrames, srcRatio);
        if (underrun) {
            prefilled = false;
            if (it >= warmupIters) ++xrunAfterWarmup;
        }

        if (it >= warmupIters) {
            fillMinTail = std::min(fillMinTail, fill);
            fillMaxTail = std::max(fillMaxTail, fill);
            fillSumTail += fill;
            ++tailSamples;
        }
    }

    char label[64];
    std::snprintf(label, sizeof(label), "driftPpm=%.0f", sc.driftPpm);

    // 補正上限(±500ppm)内のドリフトなら、リングは長期的に均衡し
    // 入力側オーバーランは起きないはず。上限を超えるドリフト
    // (700ppm シナリオ)は補正しきれず徐々に溜まっていくのが期待動作
    // なので、その場合はここでは検査しない(クラッシュしないことだけ確認)。
    if (sc.expectZeroXrunAfterWarmup && inOverrunAfterWarmup != 0) {
        Fail(std::string(label) + ": input ring overrun observed after warmup "
             "(count=" + std::to_string(inOverrunAfterWarmup) +
             ", first at iter=" + std::to_string(firstOverrunIter) + " of " +
             std::to_string(totalIters) + ", warmupIters=" +
             std::to_string(warmupIters) +
             ") (should not happen once converged, drift is within "
             "correction range)");
    }

    if (sc.expectZeroXrunAfterWarmup && xrunAfterWarmup != 0) {
        Fail(std::string(label) + ": xrun after warmup = " +
             std::to_string(xrunAfterWarmup) + " (expected 0)");
    }

    if (tailSamples == 0) Fail(std::string(label) + ": no samples collected after warmup");
    const double fillAvgTail = fillSumTail / (double)tailSamples;

    if (sc.expectZeroXrunAfterWarmup) {
        // 収束後は 50% 近辺で安定しているはず
        if (std::fabs(fillAvgTail - 0.5) > 0.05) {
            Fail(std::string(label) + ": converged fill average out of band: " +
                 std::to_string(fillAvgTail));
        }
        if (fillMinTail < 0.2 || fillMaxTail > 0.8) {
            Fail(std::string(label) + ": fill ratio excursion too large "
                                       "(min=" + std::to_string(fillMinTail) +
                 " max=" + std::to_string(fillMaxTail) + ")");
        }
    }
    // fill は定義上つねに [0,1] のはず(暴走していないことの最低限の確認)
    if (fillMinTail < 0.0 || fillMaxTail > 1.0) {
        Fail(std::string(label) + ": fill ratio left [0,1] range");
    }

    std::printf("PASS: %s  xrun_after_warmup=%lld  fill_avg=%.4f "
                "fill_range=[%.4f,%.4f]  ratio=%.7f\n",
                label, xrunAfterWarmup, fillAvgTail, fillMinTail, fillMaxTail,
                lastRatio);
}

}  // namespace

int main() {
    // 通常のデバイス間ドリフトは ±100ppm 以内(実装ガイド §4.3.3)。
    // コントローラの補正上限は ±500ppm(maxCorr_)。その範囲内は
    // xrun 0 での収束を要求し、範囲外(700ppm)は「クラッシュしない・
    // fill が [0,1] を出ない」ことだけを緩く確認する頑健性チェック。
    const Scenario scenarios[] = {
        {-400.0, true}, {-100.0, true}, {0.0, true},
        {100.0, true},  {400.0, true},  {700.0, false},
    };

    for (const auto& sc : scenarios) {
        g_rtAllocViolations.store(0);
        RunScenario(sc);
        if (g_rtAllocViolations.load() != 0) {
            Fail("allocation occurred inside RT region during driftPpm=" +
                 std::to_string(sc.driftPpm));
        }
    }

    std::printf("ALL PASS: drift/ASRC convergence\n");
    return 0;
}
