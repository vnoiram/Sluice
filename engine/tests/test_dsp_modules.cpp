// test_dsp_modules.cpp : EQ/gate/compressor/limiter の単体回帰テスト
// (実装ガイド §5.5)。実オーディオデバイス不要。既知の入力信号に対する
// 出力を検算する。

#include "dsp/biquad.h"
#include "dsp/compressor.h"
#include "dsp/delay_line.h"
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
    LimiterRuntime lim(kSampleRate);
    LimiterParams p;
    p.enabled = true;
    p.ceilingDb = -0.3f;  // linear ≈ 0.9647
    p.kneeDb = 3.0f;

    const float ceiling = std::pow(10.0f, p.ceilingDb / 20.0f);

    // lookahead 遅延線(既定 5ms @ 48kHz = 240 フレーム)より十分長いブロックを
    // 使い、遅延済みの定常状態出力を観測できるようにする。
    std::vector<float> buf(1024);
    for (size_t i = 0; i < buf.size(); ++i) buf[i] = 3.0f;  // 大幅に ceiling を超える一定値
    lim.ProcessBlock(buf.data(), (int)buf.size(), p);

    for (float v : buf) {
        if (v > ceiling + 1e-4f)
            Fail("limiter: output exceeded ceiling (" + std::to_string(v) + " > " +
                 std::to_string(ceiling) + ")");
    }
    CheckClose(buf.back(), ceiling, 0.01, "limiter clamped value");

    // ceiling 未満(ニー開始点より下)の信号は定常状態で素通しであるべき
    LimiterRuntime lim2(kSampleRate);
    std::vector<float> quiet(1024, 0.01f);
    lim2.ProcessBlock(quiet.data(), (int)quiet.size(), p);
    CheckClose(quiet.back(), 0.01, 1e-4, "limiter passthrough below knee");

    std::printf("PASS: limiter (clamped=%.4f, passthrough=%.4f)\n", buf.back(), quiet.back());
}

// ---------------------------------------------------------------------------
// Limiter lookahead: 無効時は遅延線を素通りするだけ。有効時は静寂の後に
// 大きな信号が始まる場合、その信号が実際に出力タップへ到達する時点までに
// 十分ゲインが下がっていること(= 先読みして反応できていること)を確認する。
// ---------------------------------------------------------------------------
void TestLimiterLookahead() {
    constexpr int kLookaheadFrames = 240;  // 5ms @ 48kHz(LimiterRuntime の既定値)

    LimiterRuntime limOff(kSampleRate);
    LimiterParams pOff;
    pOff.enabled = false;
    std::vector<float> passthroughBuf(600);
    for (size_t i = 0; i < passthroughBuf.size(); ++i) passthroughBuf[i] = (float)i;
    limOff.ProcessBlock(passthroughBuf.data(), (int)passthroughBuf.size(), pOff);
    CheckClose(passthroughBuf[599], 599 - kLookaheadFrames, 1e-3,
               "limiter disabled: pure delay passthrough");

    LimiterRuntime lim(kSampleRate);
    LimiterParams p;
    p.enabled = true;
    p.ceilingDb = -0.3f;
    p.kneeDb = 3.0f;
    const float ceiling = std::pow(10.0f, p.ceilingDb / 20.0f);

    constexpr int kOnsetIndex = 100;
    constexpr int kTotalFrames = 600;
    std::vector<float> signal(kTotalFrames, 0.0f);
    for (int i = kOnsetIndex; i < kTotalFrames; ++i) signal[i] = 3.0f;  // ceiling を大きく超える
    lim.ProcessBlock(signal.data(), kTotalFrames, p);

    // 大音量の立ち上がり(入力インデックス kOnsetIndex)が遅延線を通って
    // 出力に現れるのは kOnsetIndex + kLookaheadFrames サンプル目。lookahead
    // 無しであればこの時点でゲインはまだ 1.0 に近く大幅にオーバーするが、
    // ここでは静寂期間中ずっと先読みでゲインが下がり続けているため、この
    // サンプルの時点で既に ceiling 近くまで収まっているはず。
    const float atOnsetOutput = signal[(size_t)(kOnsetIndex + kLookaheadFrames)];
    if (atOnsetOutput > ceiling * 1.05f) {
        Fail("limiter lookahead: onset sample exceeded ceiling by >5% (" +
             std::to_string(atOnsetOutput) + " vs ceiling " + std::to_string(ceiling) + ")");
    }

    std::printf("PASS: limiter lookahead (onset output=%.4f, ceiling=%.4f)\n", atOnsetOutput,
                ceiling);
}

// ---------------------------------------------------------------------------
// DelayLine: offsetFrames サンプル前に書き込まれた値を正しく返すこと
// ---------------------------------------------------------------------------
void TestDelayLine() {
    DelayLine delayZero(4);
    for (int i = 0; i < 6; ++i) {
        const float out = delayZero.Push((float)i, 0);
        CheckClose(out, (float)i, 1e-6, "delay offset0 passthrough at index " + std::to_string(i));
    }

    DelayLine delay(4);  // maxOffsetFrames=4
    const std::vector<float> expected = {0, 0, 0, 0, 1, 2, 3, 4, 5, 6};
    for (int i = 0; i < 10; ++i) {
        const float out = delay.Push((float)(i + 1), 4);
        CheckClose(out, expected[(size_t)i], 1e-6, "delay offset4 at index " + std::to_string(i));
    }

    std::printf("PASS: delay line offset semantics\n");
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
// Compressor lookahead: 定常状態のゲイン計算そのものは lookahead の有無で
// 変わらないこと(遅延を追加するだけ)を確認する。
// ---------------------------------------------------------------------------
void TestCompressorLookahead() {
    CompressorRuntime comp(kSampleRate);
    CompParams p;
    p.enabled = true;
    p.thresholdDb = -20.0f;
    p.ratio = 4.0f;
    p.attackMs = 1.0f;
    p.releaseMs = 20.0f;
    p.makeupDb = 0.0f;
    p.lookaheadMs = 5.0f;  // 240 フレーム @ 48kHz

    constexpr int kBlockFrames = 512;
    constexpr int kBlocks = 200;
    std::vector<float> buf(kBlockFrames, 1.0f);
    for (int i = 0; i < kBlocks; ++i) {
        std::fill(buf.begin(), buf.end(), 1.0f);
        comp.ProcessBlock(buf.data(), kBlockFrames, p);
    }

    const float expectedLin = std::pow(10.0f, -15.0f / 20.0f);
    CheckClose(buf.back(), expectedLin, 0.02, "compressor lookahead steady-state output");

    std::printf("PASS: compressor lookahead (output=%.4f, expected=%.4f)\n", buf.back(),
                expectedLin);
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

// ---------------------------------------------------------------------------
// EQ 係数スムージング: パラメータ変更直後は目標係数へ即座に飛ばず、
// 数ブロックかけて収束することを確認する。lowShelf だけにゲインを与え、
// 他バンドは 0dB(数学的に厳密な恒等フィルタ)のままにすることで、DC
// (定常/直流)入力に対するチェーン全体のゲインが lowShelf 単体の DC ゲイン
// と一致するようにしている。
// ---------------------------------------------------------------------------
void TestEqCoefficientSmoothing() {
    EqRuntime eq(kSampleRate);
    EqParams p;
    p.enabled = true;
    p.lowShelfGainDb = 12.0f;  // peak1/peak2/highShelf は既定(0dB)のまま
    eq.SetParams(p);

    const BiquadCoeffs targetLow =
        BiquadCoeffs::LowShelf(kSampleRate, p.lowShelfFreq, p.lowShelfGainDb);
    const float targetDcGain =
        (targetLow.b0 + targetLow.b1 + targetLow.b2) / (1.0f + targetLow.a1 + targetLow.a2);

    constexpr int kBlockFrames = 64;
    std::vector<float> buf(kBlockFrames);

    std::fill(buf.begin(), buf.end(), 1.0f);
    eq.ProcessBlock(buf.data(), kBlockFrames);
    const float afterOneBlock = buf.back();
    // 1 ブロックだけでは目標値の半分より手前までしか進んでいないはず
    // (即座に飛ぶのではなく、ブロック単位で徐々に近づく設計になっているか)。
    if (std::fabs(afterOneBlock - targetDcGain) < std::fabs(1.0f - targetDcGain) * 0.5f) {
        Fail("eq smoothing: converged too fast after a single block (got " +
             std::to_string(afterOneBlock) + ", target " + std::to_string(targetDcGain) + ")");
    }

    for (int i = 0; i < 200; ++i) {
        std::fill(buf.begin(), buf.end(), 1.0f);
        eq.ProcessBlock(buf.data(), kBlockFrames);
    }
    CheckClose(buf.back(), targetDcGain, 0.05, "eq smoothing converged value");

    std::printf("PASS: eq coefficient smoothing (after1block=%.4f, converged=%.4f, target=%.4f)\n",
                afterOneBlock, buf.back(), targetDcGain);
}

}  // namespace

int main() {
    TestBiquadIdentity();
    TestEqCoefficientSmoothing();
    TestLimiter();
    TestLimiterLookahead();
    TestGate();
    TestCompressor();
    TestCompressorLookahead();
    TestDelayLine();
    std::printf("ALL PASS: dsp modules (biquad/eq/limiter/gate/compressor/delay_line)\n");
    return 0;
}
