#pragma once
// biquad.h : RBJ(Audio EQ Cookbook)biquad フィルタ(実装ガイド §5.5)
//
// 係数は制御スレッド/ブロック先頭で計算し、状態(z1,z2)だけ RT が持つ
// (実装ガイド §5.5)。Direct Form II Transposed を採用。
//
// 係数切替時のクロスフェード/スムージングは呼び出し側(dsp/eq.h の
// EqRuntime)が dsp/smoother.h の SmoothingCoeff を使ってブロック単位で
// 行う。BiquadState/BiquadCoeffs 自体は「渡された係数でそのまま処理する」
// だけの薄いプリミティブに留める。

#include <cmath>

struct BiquadCoeffs {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;  // a0 で正規化済み

    static BiquadCoeffs Identity() { return BiquadCoeffs{}; }

    static BiquadCoeffs LowShelf(float sampleRate, float freq, float gainDb,
                                 float shelfSlope = 1.0f) {
        const float A = std::pow(10.0f, gainDb / 40.0f);
        const float w0 = 2.0f * 3.14159265358979323846f * freq / sampleRate;
        const float cosw0 = std::cos(w0), sinw0 = std::sin(w0);
        const float alpha =
            sinw0 / 2.0f * std::sqrt((A + 1.0f / A) * (1.0f / shelfSlope - 1.0f) + 2.0f);
        const float twoSqrtAalpha = 2.0f * std::sqrt(A) * alpha;

        const float b0 = A * ((A + 1) - (A - 1) * cosw0 + twoSqrtAalpha);
        const float b1 = 2 * A * ((A - 1) - (A + 1) * cosw0);
        const float b2 = A * ((A + 1) - (A - 1) * cosw0 - twoSqrtAalpha);
        const float a0 = (A + 1) + (A - 1) * cosw0 + twoSqrtAalpha;
        const float a1 = -2 * ((A - 1) + (A + 1) * cosw0);
        const float a2 = (A + 1) + (A - 1) * cosw0 - twoSqrtAalpha;
        return Normalize(b0, b1, b2, a0, a1, a2);
    }

    static BiquadCoeffs HighShelf(float sampleRate, float freq, float gainDb,
                                  float shelfSlope = 1.0f) {
        const float A = std::pow(10.0f, gainDb / 40.0f);
        const float w0 = 2.0f * 3.14159265358979323846f * freq / sampleRate;
        const float cosw0 = std::cos(w0), sinw0 = std::sin(w0);
        const float alpha =
            sinw0 / 2.0f * std::sqrt((A + 1.0f / A) * (1.0f / shelfSlope - 1.0f) + 2.0f);
        const float twoSqrtAalpha = 2.0f * std::sqrt(A) * alpha;

        const float b0 = A * ((A + 1) + (A - 1) * cosw0 + twoSqrtAalpha);
        const float b1 = -2 * A * ((A - 1) + (A + 1) * cosw0);
        const float b2 = A * ((A + 1) + (A - 1) * cosw0 - twoSqrtAalpha);
        const float a0 = (A + 1) - (A - 1) * cosw0 + twoSqrtAalpha;
        const float a1 = 2 * ((A - 1) - (A + 1) * cosw0);
        const float a2 = (A + 1) - (A - 1) * cosw0 - twoSqrtAalpha;
        return Normalize(b0, b1, b2, a0, a1, a2);
    }

    static BiquadCoeffs Peaking(float sampleRate, float freq, float gainDb, float q) {
        const float A = std::pow(10.0f, gainDb / 40.0f);
        const float w0 = 2.0f * 3.14159265358979323846f * freq / sampleRate;
        const float cosw0 = std::cos(w0), sinw0 = std::sin(w0);
        const float alpha = sinw0 / (2.0f * q);

        const float b0 = 1 + alpha * A;
        const float b1 = -2 * cosw0;
        const float b2 = 1 - alpha * A;
        const float a0 = 1 + alpha / A;
        const float a1 = -2 * cosw0;
        const float a2 = 1 - alpha / A;
        return Normalize(b0, b1, b2, a0, a1, a2);
    }

private:
    static BiquadCoeffs Normalize(float b0, float b1, float b2, float a0, float a1, float a2) {
        BiquadCoeffs c;
        c.b0 = b0 / a0;
        c.b1 = b1 / a0;
        c.b2 = b2 / a0;
        c.a1 = a1 / a0;
        c.a2 = a2 / a0;
        return c;
    }
};

// 状態(z1, z2)だけを持つ。係数は毎サンプル呼び出し側から渡す。
class BiquadState {
public:
    float ProcessSample(float x, const BiquadCoeffs& c) {
        const float y = c.b0 * x + z1_;
        z1_ = c.b1 * x - c.a1 * y + z2_;
        z2_ = c.b2 * x - c.a2 * y;
        return y;
    }
    void Reset() { z1_ = z2_ = 0.0f; }

private:
    float z1_ = 0.0f, z2_ = 0.0f;
};
