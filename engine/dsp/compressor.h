#pragma once
// compressor.h : コンプレッサ(実装ガイド §5.5)
// エンベロープ(遅延前の生信号から算出)→ dB 変換 → レシオ適用 →
// メイクアップ。lookaheadMs > 0 のとき、遅延線(delay_line.h)を通した
// 乾信号にゲインを適用することで、実際の音量変化を先取りして反応できる
// (envelope_ 自体は従来どおり遅延なしの生信号を追跡し続ける)。

#include <algorithm>
#include <cmath>

#include "dsp/delay_line.h"

struct CompParams {
    bool enabled = false;
    float thresholdDb = -18.0f;
    float ratio = 2.0f;  // N:1 の N
    float attackMs = 5.0f;
    float releaseMs = 80.0f;
    float makeupDb = 0.0f;
    float lookaheadMs = 0.0f;  // 0 = lookaheadなし(従来どおりの瞬時処理、後方互換の既定値)
};

class CompressorRuntime {
public:
    // maxLookaheadMs: 構築時に確保する遅延線の最大長。CompParams::lookaheadMs
    // はこれを超える値を要求してもクランプされる(DelayLine は構築時に
    // 1回だけ確保する RT安全設計のため、上限は実行中に拡張できない)。
    explicit CompressorRuntime(float sampleRate, float maxLookaheadMs = 20.0f)
        : sampleRate_(sampleRate),
          maxLookaheadFrames_(ToFrames(sampleRate, maxLookaheadMs)),
          delay_(maxLookaheadFrames_) {}

    void ProcessBlock(float* buf, int frames, const CompParams& p) {
        const int lookaheadFrames =
            std::min(ToFrames(sampleRate_, p.lookaheadMs), maxLookaheadFrames_);

        if (!p.enabled) {
            // 有効/無効の切替で出力の位相がずれないよう、無効時も同じだけ
            // 遅延線を進めておく。
            for (int i = 0; i < frames; ++i) buf[i] = delay_.Push(buf[i], lookaheadFrames);
            return;
        }
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

            const float delayed = delay_.Push(in, lookaheadFrames);
            buf[i] = delayed * gainLin * makeupLin;
        }
    }

private:
    static float LinearToDb(float lin) { return 20.0f * std::log10(std::max(lin, 1e-9f)); }
    float AttackReleaseCoeff(float ms) const {
        if (ms <= 0.0f) return 1.0f;
        return 1.0f - std::exp(-1.0f / (0.001f * ms * sampleRate_));
    }
    static int ToFrames(float sampleRate, float ms) {
        const float f = ms * 0.001f * sampleRate;
        return f > 0.0f ? static_cast<int>(f) : 0;
    }

    float sampleRate_;
    int maxLookaheadFrames_;
    DelayLine delay_;
    float envelope_ = 0.0f;
};
