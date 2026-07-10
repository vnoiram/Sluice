#pragma once
// eq.h : 4 バンド EQ(Low-shelf / Peaking ×2 / High-shelf の直列、実装ガイド §5.5)

#include <cstring>

#include "dsp/biquad.h"

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
        lowShelf_ = BiquadCoeffs::LowShelf(sampleRate_, p.lowShelfFreq, p.lowShelfGainDb);
        peak1_ = BiquadCoeffs::Peaking(sampleRate_, p.peak1Freq, p.peak1GainDb, p.peak1Q);
        peak2_ = BiquadCoeffs::Peaking(sampleRate_, p.peak2Freq, p.peak2GainDb, p.peak2Q);
        highShelf_ = BiquadCoeffs::HighShelf(sampleRate_, p.highShelfFreq, p.highShelfGainDb);
    }

    void ProcessBlock(float* buf, int frames) {
        if (!enabled_) return;
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
    float sampleRate_;
    bool enabled_ = false;
    EqParams lastParams_{};
    BiquadCoeffs lowShelf_, peak1_, peak2_, highShelf_;
    BiquadState lowState_, peak1State_, peak2State_, highState_;
};
