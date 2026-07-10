#pragma once
// limiter.h : バス用リミッタ(実装ガイド §5.5)
// 最初はハードクリップ+ソフトニー。v2 で lookahead 型に(未実装)。

#include <cmath>

struct LimiterParams {
    bool enabled = true;
    float ceilingDb = -0.3f;  // 出力上限
    float kneeDb = 3.0f;      // ソフトニー幅
};

class LimiterRuntime {
public:
    void ProcessBlock(float* buf, int frames, const LimiterParams& p) {
        if (!p.enabled) return;
        const float ceiling = std::pow(10.0f, p.ceilingDb / 20.0f);
        const float kneeStart = ceiling * std::pow(10.0f, -p.kneeDb / 20.0f);
        const float kneeRange = ceiling - kneeStart;

        for (int i = 0; i < frames; ++i) {
            const float x = buf[i];
            const float sign = x < 0.0f ? -1.0f : 1.0f;
            const float mag = std::fabs(x);
            float outMag;
            if (mag <= kneeStart || kneeRange <= 0.0f) {
                outMag = mag <= ceiling ? mag : ceiling;
            } else if (mag >= ceiling) {
                outMag = ceiling;  // ハードクリップ
            } else {
                // kneeStart〜ceiling を滑らかに ceiling へ漸近させるソフトニー
                const float t = (mag - kneeStart) / kneeRange;
                outMag = kneeStart + kneeRange * (1.0f - (1.0f - t) * (1.0f - t));
            }
            buf[i] = sign * outMag;
        }
    }
};
