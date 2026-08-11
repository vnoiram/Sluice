// test_spsc_ring.cpp : SpscRing の 1 プロデューサ・1 コンシューマ ストレステスト
//
// 検証すること:
//   1. 生産順序どおりに、欠落・重複・破損なく消費側へ届くこと
//      (単一要素の Write/Read、および可変長バッチの Write/Read の両方で)
//   2. RT スレッドに相当する区間(Write/Read の呼び出し中)で
//      メモリアロケーションが 0 件であること(実装ガイド §1.4 / §11)
//   3. デッドロック/ライブロックしないこと(ウォッチドッグで検出)
//
// 実際の ASIO コールバック(asio_host.cpp の OnBufferSwitch)からは
// このリングの Write/Read だけが呼ばれるため、ここでの検証が
// そのまま RT 安全性の裏付けになる。

#include "rt/spsc_ring.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>
#include <thread>
#include <vector>

// ===========================================================================
// RT 区間アロケーション検出フック
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

// 失敗したら詳細を出して即終了(assert はリリースビルドで消えるため使わない)
void Check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

// producer/consumer がライブロック/デッドロックした場合に検出して落とす。
// doneFlag が timeoutSeconds 以内に立たなければプロセスを異常終了させる。
class Watchdog {
public:
    Watchdog(std::atomic<bool>& doneFlag, int timeoutSeconds, const char* label)
        : thread_([&doneFlag, timeoutSeconds, label] {
              const auto deadline = std::chrono::steady_clock::now() +
                                     std::chrono::seconds(timeoutSeconds);
              while (!doneFlag.load(std::memory_order_acquire)) {
                  if (std::chrono::steady_clock::now() > deadline) {
                      std::fprintf(stderr,
                                    "FAIL: watchdog timeout (%ds) in %s "
                                    "(deadlock/livelock suspected)\n",
                                    timeoutSeconds, label);
                      std::abort();
                  }
                  std::this_thread::sleep_for(std::chrono::milliseconds(200));
              }
          }) {}
    // detach しない: done フラグ(呼び出し元のローカル変数)が破棄される前に
    // 監視スレッドが必ず抜けるよう join する(参照の dangling を防ぐ)。
    ~Watchdog() { thread_.join(); }

private:
    std::thread thread_;
};

// ---------------------------------------------------------------------------
// フェーズ 1: 単一要素の Write/Read で順序保証を確認
// ---------------------------------------------------------------------------
void RunSingleItemPhase() {
    constexpr size_t kCapacity = 4096;
    constexpr uint64_t kCount = 1'000'000;

    SpscRing<uint64_t> ring(kCapacity);
    std::atomic<bool> done{false};

    std::thread producer([&] {
        for (uint64_t i = 0; i < kCount; ++i) {
            uint64_t v = i;
            for (;;) {
                RtGuard g;
                size_t n = ring.Write(&v, 1);
                if (n == 1) break;
                (void)g;
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&] {
        uint64_t expected = 0;
        while (expected < kCount) {
            uint64_t v;
            size_t n;
            {
                RtGuard g;
                n = ring.Read(&v, 1);
            }
            if (n == 0) {
                std::this_thread::yield();
                continue;
            }
            Check(v == expected, "single-item: out-of-order or corrupted value");
            ++expected;
        }
    });

    Watchdog watchdog(done, 60, "single-item phase");

    producer.join();
    consumer.join();
    done.store(true, std::memory_order_release);

    Check(g_rtAllocViolations.load() == 0,
          "single-item phase: allocation occurred inside RT region");
    std::printf("PASS: single-item phase (%llu items, order preserved, "
                "0 allocations in RT region)\n",
                (unsigned long long)kCount);
}

// ---------------------------------------------------------------------------
// フェーズ 2: 可変長バッチの Write/Read (ASIO のブロック転送を模擬)
// ---------------------------------------------------------------------------
void RunBatchPhase() {
    constexpr size_t kCapacity = 8192;
    constexpr uint64_t kTotalFrames = 4'000'000;

    SpscRing<uint64_t> ring(kCapacity);
    std::atomic<bool> done{false};

    std::thread producer([&] {
        std::mt19937 rng(12345);
        std::uniform_int_distribution<int> sizeDist(1, 64);
        std::vector<uint64_t> chunk(64);

        uint64_t next = 0;
        while (next < kTotalFrames) {
            int want = (int)std::min<uint64_t>((uint64_t)sizeDist(rng),
                                                kTotalFrames - next);
            for (int i = 0; i < want; ++i) chunk[i] = next + i;

            size_t off = 0;
            while (off < (size_t)want) {
                RtGuard g;
                size_t n = ring.Write(chunk.data() + off, (size_t)want - off);
                (void)g;
                off += n;
                if (n == 0) std::this_thread::yield();
            }
            next += (uint64_t)want;
        }
    });

    std::thread consumer([&] {
        std::mt19937 rng(6789);
        std::uniform_int_distribution<int> sizeDist(1, 64);
        std::vector<uint64_t> chunk(64);

        uint64_t expected = 0;
        while (expected < kTotalFrames) {
            int want = sizeDist(rng);
            size_t n;
            {
                RtGuard g;
                n = ring.Read(chunk.data(), (size_t)want);
            }
            if (n == 0) {
                std::this_thread::yield();
                continue;
            }
            for (size_t i = 0; i < n; ++i) {
                Check(chunk[i] == expected + i,
                      "batch phase: out-of-order or corrupted value");
            }
            expected += n;
        }
    });

    Watchdog watchdog(done, 60, "batch phase");

    producer.join();
    consumer.join();
    done.store(true, std::memory_order_release);

    Check(g_rtAllocViolations.load() == 0,
          "batch phase: allocation occurred inside RT region");
    std::printf("PASS: batch phase (%llu frames, order preserved, "
                "0 allocations in RT region)\n",
                (unsigned long long)kTotalFrames);
}

}  // namespace

int main() {
    RunSingleItemPhase();
    // フェーズ間でカウンタをリセットして、各フェーズを独立に検証する
    g_rtAllocViolations.store(0);
    RunBatchPhase();
    std::printf("ALL PASS: spsc_ring\n");
    return 0;
}
