#pragma once
// limiter.h : バス用リミッタ(実装ガイド §5.5)
// ソフトニー + lookahead。ゲインは遅延前(先読みできる)信号から算出し、
// 遅延線(delay_line.h)を通した乾信号に適用する — 実際に大きな音が
// 出力タップへ到達する前にゲイン低下を開始できる。

#include <cmath>

#include "dsp/delay_line.h"

struct LimiterParams {
    bool enabled = true;
    float ceilingDb = -0.3f;  // 出力上限
    float kneeDb = 3.0f;      // ソフトニー幅
};

class LimiterRuntime {
public:
    // lookaheadMs: 遅延線の長さ(構築時に1回だけ確保。実行中には変更しない
    // ミニマム実装の割り切り — 可変にしたい場合は DelayLine を
    // maxLookaheadMs で確保し compressor.h と同様に毎ブロッククランプする
    // 形に拡張できる)。
    explicit LimiterRuntime(float sampleRate, float lookaheadMs = 5.0f)
        : sampleRate_(sampleRate),
          lookaheadFrames_(ToFrames(sampleRate, lookaheadMs)),
          delay_(lookaheadFrames_) {}

    void ProcessBlock(float* buf, int frames, const LimiterParams& p) {
        if (!p.enabled) {
            // 無効時も遅延線は素通りさせる(有効/無効の切替で出力の位相が
            // ずれないよう、常に同じ量だけ遅延させる)。ゲインは 1.0 として
            // 再開に備えてリセットする。
            for (int i = 0; i < frames; ++i) buf[i] = delay_.Push(buf[i], lookaheadFrames_);
            gainLin_ = 1.0f;
            return;
        }
        const float ceiling = std::pow(10.0f, p.ceilingDb / 20.0f);
        const float kneeStart = ceiling * std::pow(10.0f, -p.kneeDb / 20.0f);
        const float kneeRange = ceiling - kneeStart;
        // attack は lookahead 窓内で十分収束するよう、lookaheadMs より
        // 明確に短い固定値にする。既定の lookaheadMs(5ms)に対して
        // kAttackMs=0.5ms なら窓内で 10 時定数分経過し、残差 exp(-10) ≈
        // 4.5e-5 まで収束するため、実用上はブリックウォールとみなせる
        // (envelope ベースのゲイン制御である以上、数学的に厳密な保証では
        // ない点には留意)。
        const float attackCoeff = PerSampleCoeff(kAttackMs);
        const float releaseCoeff = PerSampleCoeff(kReleaseMs);

        for (int i = 0; i < frames; ++i) {
            const float x = buf[i];
            const float mag = std::fabs(x);
            float targetOutMag;
            if (mag <= kneeStart || kneeRange <= 0.0f) {
                targetOutMag = mag <= ceiling ? mag : ceiling;
            } else if (mag >= ceiling) {
                targetOutMag = ceiling;  // ハードクリップ
            } else {
                // kneeStart〜ceiling を滑らかに ceiling へ漸近させるソフトニー
                const float t = (mag - kneeStart) / kneeRange;
                targetOutMag = kneeStart + kneeRange * (1.0f - (1.0f - t) * (1.0f - t));
            }
            const float targetGain = mag > 1e-9f ? (targetOutMag / mag) : 1.0f;

            // ゲインを下げる方向(attack)は速く、戻す方向(release)は
            // ゆっくり追従させる。遅延前の生信号から算出するため、実際に
            // 大きな音が delay_ から出てくる lookaheadFrames_ サンプル後には
            // ゲインが既に下がり切っている。
            const float coeff = (targetGain < gainLin_) ? attackCoeff : releaseCoeff;
            gainLin_ += (targetGain - gainLin_) * coeff;

            const float delayed = delay_.Push(x, lookaheadFrames_);
            buf[i] = delayed * gainLin_;
        }
    }

private:
    static int ToFrames(float sampleRate, float ms) {
        const float f = ms * 0.001f * sampleRate;
        return f > 0.0f ? static_cast<int>(f) : 0;
    }
    float PerSampleCoeff(float ms) const {
        if (ms <= 0.0f) return 1.0f;
        return 1.0f - std::exp(-1.0f / (0.001f * ms * sampleRate_));
    }

    static constexpr float kAttackMs = 0.5f;
    static constexpr float kReleaseMs = 50.0f;

    float sampleRate_;
    int lookaheadFrames_;
    DelayLine delay_;
    float gainLin_ = 1.0f;
};
