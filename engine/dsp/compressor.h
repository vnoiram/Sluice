#pragma once
// compressor.h : コンプレッサ(実装ガイド §5.5)
// エンベロープ → dB 変換 → レシオ適用 → メイクアップ。lookahead は
// 実装ガイドの指示どおり後回し(未実装)。

#include <algorithm>
#include <cmath>

struct CompParams {
    bool enabled = false;
    float thresholdDb = -18.0f;
    float ratio = 2.0f;  // N:1 の N
    float attackMs = 5.0f;
    float releaseMs = 80.0f;
    float makeupDb = 0.0f;
};

class CompressorRuntime {
public:
    explicit CompressorRuntime(float sampleRate) : sampleRate_(sampleRate) {}

    void ProcessBlock(float* buf, int frames, const CompParams& p) {
        if (!p.enabled) return;
        const float attackCoeff = AttackReleaseCoeff(p.attackMs);
        const float releaseCoeff = AttackReleaseCoeff(p.releaseMs);
        const float makeupLin = std::pow(10.0f, p.makeupDb / 20.0f);
        const float ratio = p.ratio > 0.0f ? p.ratio : 1.0f;

        for (int i = 0; i < frames; ++i) {
            const float in = buf[i];
            const float rectified = std::fabs(in);
            const float coeff = (rectified > envelope_) ? attackCoeff : releaseCoeff;
            envelope_ += (rectified - envelope_) * coeff;

            const float envDb = LinearToDb(envelope_);
            float gainReductionDb = 0.0f;
            if (envDb > p.thresholdDb) {
                const float over = envDb - p.thresholdDb;
                gainReductionDb = over - over / ratio;
            }
            const float gainLin = std::pow(10.0f, -gainReductionDb / 20.0f);
            buf[i] = in * gainLin * makeupLin;
        }
    }

private:
    static float LinearToDb(float lin) { return 20.0f * std::log10(std::max(lin, 1e-9f)); }
    float AttackReleaseCoeff(float ms) const {
        if (ms <= 0.0f) return 1.0f;
        return 1.0f - std::exp(-1.0f / (0.001f * ms * sampleRate_));
    }

    float sampleRate_;
    float envelope_ = 0.0f;
};
