#pragma once
// eq.h : 4 バンド EQ(Low-shelf / Peaking ×2 / High-shelf の直列、実装ガイド §5.5)

#include <cstring>

#include "dsp/biquad.h"
#include "dsp/smoother.h"

// ヒープ確保を伴うメンバを持たない POD 的な構造体(TripleBuffer/StripParams
// に埋め込んでコピーするため)。
struct EqParams {
    bool enabled = false;
    float lowShelfFreq = 100.0f, lowShelfGainDb = 0.0f;
    float peak1Freq = 500.0f, peak1GainDb = 0.0f, peak1Q = 1.0f;
    float peak2Freq = 3000.0f, peak2GainDb = 0.0f, peak2Q = 1.0f;
    float highShelfFreq = 8000.0f, highShelfGainDb = 0.0f;
};

class EqRuntime {
public:
    // サンプルレートは構築時に固定(実行中の変更は非対応。ミニマム実装の
    // 意図的な割り切り)。
    explicit EqRuntime(float sampleRate) : sampleRate_(sampleRate) {}

    // ブロック先頭で呼ぶ。パラメータが前回と同じならフィルタ係数の
    // 再計算(三角関数呼び出し)をスキップする。
    void SetParams(const EqParams& p) {
        if (std::memcmp(&p, &lastParams_, sizeof(EqParams)) == 0) return;
        lastParams_ = p;
        enabled_ = p.enabled;
        if (!enabled_) return;
        // 実際にフィルタへ渡す current* はここでは更新しない — target* だけ
        // 更新し、ProcessBlock がブロックごとに少しずつ近づける(biquad.h の
        // 「係数切替時はクロスフェードか係数スムージング」対応。急な係数の
        // 上書きによるクリックを避ける)。
        targetLowShelf_ = BiquadCoeffs::LowShelf(sampleRate_, p.lowShelfFreq, p.lowShelfGainDb);
        targetPeak1_ = BiquadCoeffs::Peaking(sampleRate_, p.peak1Freq, p.peak1GainDb, p.peak1Q);
        targetPeak2_ = BiquadCoeffs::Peaking(sampleRate_, p.peak2Freq, p.peak2GainDb, p.peak2Q);
        targetHighShelf_ = BiquadCoeffs::HighShelf(sampleRate_, p.highShelfFreq, p.highShelfGainDb);
    }

    void ProcessBlock(float* buf, int frames) {
        if (!enabled_) return;
        const float alpha = SmoothingCoeff(kSmoothMs, frames, sampleRate_);
        RampToward(lowShelf_, targetLowShelf_, alpha);
        RampToward(peak1_, targetPeak1_, alpha);
        RampToward(peak2_, targetPeak2_, alpha);
        RampToward(highShelf_, targetHighShelf_, alpha);

        for (int i = 0; i < frames; ++i) {
            float x = buf[i];
            x = lowState_.ProcessSample(x, lowShelf_);
            x = peak1State_.ProcessSample(x, peak1_);
            x = peak2State_.ProcessSample(x, peak2_);
            x = highState_.ProcessSample(x, highShelf_);
            buf[i] = x;
        }
    }

private:
    static void RampToward(BiquadCoeffs& current, const BiquadCoeffs& target, float alpha) {
        current.b0 += (target.b0 - current.b0) * alpha;
        current.b1 += (target.b1 - current.b1) * alpha;
        current.b2 += (target.b2 - current.b2) * alpha;
        current.a1 += (target.a1 - current.a1) * alpha;
        current.a2 += (target.a2 - current.a2) * alpha;
    }

    // 係数スムージングの時定数。数ミリ秒でクリックを消すには十分だが、
    // パラメータ変更への追従が遅れて聞こえるほど長くはない値。
    static constexpr float kSmoothMs = 8.0f;

    float sampleRate_;
    bool enabled_ = false;
    EqParams lastParams_{};
    BiquadCoeffs lowShelf_, peak1_, peak2_, highShelf_;  // 実際にフィルタへ渡す現在値
    BiquadCoeffs targetLowShelf_, targetPeak1_, targetPeak2_, targetHighShelf_;  // 目標値
    BiquadState lowState_, peak1State_, peak2State_, highState_;
};
