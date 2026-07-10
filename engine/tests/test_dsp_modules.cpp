// test_dsp_modules.cpp : EQ/gate/compressor/limiter の単体回帰テスト
// (実装ガイド §5.5)。実オーディオデバイス不要。既知の入力信号に対する
// 出力を検算する。

#include "dsp/biquad.h"
#include "dsp/compressor.h"
#include "dsp/eq.h"
#include "dsp/gate.h"
#include "dsp/limiter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr float kSampleRate = 48000.0f;

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

// ---------------------------------------------------------------------------
// Limiter: 上限(ceiling)を超える信号は ceiling までクランプされる
// ---------------------------------------------------------------------------
void TestLimiter() {
    LimiterRuntime lim;
    LimiterParams p;
    p.enabled = true;
    p.ceilingDb = -0.3f;  // linear ≈ 0.9647
    p.kneeDb = 3.0f;

    const float ceiling = std::pow(10.0f, p.ceilingDb / 20.0f);

    std::vector<float> buf(512);
    for (size_t i = 0; i < buf.size(); ++i) buf[i] = 3.0f;  // 大幅に ceiling を超える一定値
    lim.ProcessBlock(buf.data(), (int)buf.size(), p);

    for (float v : buf) {
        if (v > ceiling + 1e-4f)
            Fail("limiter: output exceeded ceiling (" + std::to_string(v) + " > " +
                 std::to_string(ceiling) + ")");
    }
    CheckClose(buf.back(), ceiling, 0.01, "limiter clamped value");

    // ceiling 未満(ニー開始点より下)の信号は素通しであるべき
    LimiterRuntime lim2;
    std::vector<float> quiet(64, 0.01f);
    lim2.ProcessBlock(quiet.data(), (int)quiet.size(), p);
    CheckClose(quiet.back(), 0.01, 1e-4, "limiter passthrough below knee");

    std::printf("PASS: limiter (clamped=%.4f, passthrough=%.4f)\n", buf.back(), quiet.back());
}

// ---------------------------------------------------------------------------
// Gate: 閾値以下の信号は十分な時間の後に減衰し、閾値以上は通過する
// ---------------------------------------------------------------------------
void TestGate() {
    GateRuntime gate(kSampleRate);
    GateParams p;
    p.enabled = true;
    p.thresholdDb = -20.0f;  // linear ≈ 0.1
    p.hysteresisDb = 3.0f;
    p.attackMs = 1.0f;
    p.releaseMs = 20.0f;

    // 十分に大きい信号(閾値超え)を長時間流し、ゲートが開いて素通しに
    // 近づくことを確認する。
    constexpr int kBlockFrames = 512;
    constexpr int kBlocks = 50;  // 50*512/48000 ≈ 0.53 秒
    std::vector<float> loud(kBlockFrames, 0.5f);
    for (int i = 0; i < kBlocks; ++i) gate.ProcessBlock(loud.data(), kBlockFrames, p);
    CheckClose(loud.back(), 0.5, 0.05, "gate open passthrough");

    // 十分に小さい信号(閾値未満)を長時間流し、ゲートが閉じて出力が
    // ほぼ 0 に近づくことを確認する。
    GateRuntime gate2(kSampleRate);
    std::vector<float> quiet(kBlockFrames, 0.001f);
    for (int i = 0; i < kBlocks; ++i) gate2.ProcessBlock(quiet.data(), kBlockFrames, p);
    if (std::fabs(quiet.back()) > 0.001f * 0.5f)
        Fail("gate: expected closed gate to attenuate signal, got " +
             std::to_string(quiet.back()));

    std::printf("PASS: gate (open=%.4f, closed=%.6f)\n", loud.back(), quiet.back());
}

// ---------------------------------------------------------------------------
// Compressor: 閾値超過分がレシオどおりに圧縮される(定常状態で検算)
// ---------------------------------------------------------------------------
void TestCompressor() {
    CompressorRuntime comp(kSampleRate);
    CompParams p;
    p.enabled = true;
    p.thresholdDb = -20.0f;
    p.ratio = 4.0f;  // 4:1
    p.attackMs = 1.0f;
    p.releaseMs = 20.0f;
    p.makeupDb = 0.0f;

    // 入力 0dB(1.0)を定常状態まで流す。
    // 超過 = 0 - (-20) = 20dB、圧縮後の超過 = 20/4 = 5dB
    // → 出力レベル = 閾値 + 5dB = -15dB
    constexpr int kBlockFrames = 512;
    constexpr int kBlocks = 200;  // 十分に envelope を収束させる
    std::vector<float> buf(kBlockFrames, 1.0f);
    for (int i = 0; i < kBlocks; ++i) {
        // ProcessBlock は buf を in-place で書き換えるため、毎ブロック
        // 「連続した 0dB 入力」を模すには呼び出し前に入力値へ戻す必要が
        // ある(戻さないと前ブロックの圧縮後の出力が次ブロックの入力に
        // なり、圧縮が繰り返し複合されてしまう)。
        std::fill(buf.begin(), buf.end(), 1.0f);
        comp.ProcessBlock(buf.data(), kBlockFrames, p);
    }

    const float expectedLin = std::pow(10.0f, -15.0f / 20.0f);
    CheckClose(buf.back(), expectedLin, 0.02, "compressor steady-state output");

    std::printf("PASS: compressor (output=%.4f, expected=%.4f)\n", buf.back(), expectedLin);
}

// ---------------------------------------------------------------------------
// Biquad: Identity 係数は信号を変化させない
// ---------------------------------------------------------------------------
void TestBiquadIdentity() {
    BiquadState state;
    BiquadCoeffs identity = BiquadCoeffs::Identity();
    for (int i = 0; i < 100; ++i) {
        float x = (float)i * 0.01f - 0.5f;
        float y = state.ProcessSample(x, identity);
        if (std::fabs(y - x) > 1e-6f)
            Fail("biquad identity: input/output mismatch at sample " + std::to_string(i));
    }
    std::printf("PASS: biquad identity passthrough\n");
}

}  // namespace

int main() {
    TestBiquadIdentity();
    TestLimiter();
    TestGate();
    TestCompressor();
    std::printf("ALL PASS: dsp modules (biquad/limiter/gate/compressor)\n");
    return 0;
}
