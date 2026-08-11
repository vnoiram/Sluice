// test_xcorr.cpp : xcorr.h のオフライン回帰テスト(実装ガイド §7.3)。
// Windows API に依存しないため、この計画で唯一自動実行可能な latencybench
// のテスト。既知の遅延+インパルス/M系列に対する検出精度を検証する。

#include <cstdio>
#include <cstdlib>
#include <random>

#include "../xcorr.h"

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "FAIL: %s (line %d)\n", #cond, __LINE__); \
            std::exit(1);                                                  \
        }                                                                  \
    } while (0)

static void TestImpulseGeneration() {
    auto sig = latencybench::GenerateImpulse(100, 42, 0.75f);
    CHECK(sig.size() == 100);
    for (size_t i = 0; i < sig.size(); ++i) {
        if (i == 42)
            CHECK(sig[i] == 0.75f);
        else
            CHECK(sig[i] == 0.0f);
    }
}

static void TestImpulseRoundTripOffset() {
    // 既知の遅延(例: 137 サンプル)ぶん後ろにインパルスをずらした
    // "録音結果" を作り、相互相関で元の遅延が復元できることを確認する。
    constexpr size_t kRefLen = 256;
    constexpr size_t kDelay = 137;
    constexpr size_t kTrailing = 512;

    auto reference = latencybench::GenerateImpulse(kRefLen, 0, 1.0f);
    std::vector<float> recorded(kDelay, 0.0f);
    recorded.insert(recorded.end(), reference.begin(), reference.end());
    recorded.resize(recorded.size() + kTrailing, 0.0f);

    size_t detected =
        latencybench::FindOffsetByCrossCorrelation(reference, recorded, kDelay + kTrailing);
    CHECK(detected == kDelay);
}

static void TestMlsGeneration() {
    auto mls = latencybench::GenerateMls(7);
    CHECK(mls.size() == 127);  // 2^7 - 1
    for (float v : mls) CHECK(v == 1.0f || v == -1.0f);

    // 未対応 order は空を返す。
    CHECK(latencybench::GenerateMls(1).empty());
    CHECK(latencybench::GenerateMls(20).empty());
}

static void TestMlsRoundTripOffsetWithNoise() {
    // M 系列はインパルスよりノイズに頑健なはず: ノイズを乗せた録音結果でも
    // 正しいオフセットを検出できることを確認する。
    constexpr size_t kDelay = 300;
    constexpr size_t kTrailing = 200;

    auto reference = latencybench::GenerateMls(9);  // 511 サンプル
    std::vector<float> recorded(kDelay, 0.0f);
    recorded.insert(recorded.end(), reference.begin(), reference.end());
    recorded.resize(recorded.size() + kTrailing, 0.0f);

    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> noise(-0.05f, 0.05f);
    for (float& v : recorded) v += noise(rng);

    size_t detected =
        latencybench::FindOffsetByCrossCorrelation(reference, recorded, kDelay + kTrailing);
    CHECK(detected == kDelay);
}

int main() {
    TestImpulseGeneration();
    TestImpulseRoundTripOffset();
    TestMlsGeneration();
    TestMlsRoundTripOffsetWithNoise();
    std::printf("test_xcorr: OK\n");
    return 0;
}
