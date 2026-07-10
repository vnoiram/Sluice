#pragma once
// gate.h : ノイズゲート(実装ガイド §5.5)
// エンベロープフォロワ(attack/release)+ ヒステリシス付き閾値。

#include <cmath>

struct GateParams {
    bool enabled = false;
    float thresholdDb = -40.0f;
    float hysteresisDb = 3.0f;  // 開いた後、閉じるまでの余裕
    float attackMs = 2.0f;
    float releaseMs = 100.0f;
};

class GateRuntime {
public:
    explicit GateRuntime(float sampleRate) : sampleRate_(sampleRate) {}

    void ProcessBlock(float* buf, int frames, const GateParams& p) {
        if (!p.enabled) return;
        const float attackCoeff = AttackReleaseCoeff(p.attackMs);
        const float releaseCoeff = AttackReleaseCoeff(p.releaseMs);
        const float openThresh = std::pow(10.0f, p.thresholdDb / 20.0f);
        const float closeThresh = std::pow(10.0f, (p.thresholdDb - p.hysteresisDb) / 20.0f);

        for (int i = 0; i < frames; ++i) {
            const float in = buf[i];
            const float rectified = std::fabs(in);
            const float coeff = (rectified > envelope_) ? attackCoeff : releaseCoeff;
            envelope_ += (rectified - envelope_) * coeff;

            if (isOpen_) {
                if (envelope_ < closeThresh) isOpen_ = false;
            } else {
                if (envelope_ > openThresh) isOpen_ = true;
            }

            // 開閉の急変によるクリックを避けるための軽いゲインスムージング
            const float target = isOpen_ ? 1.0f : 0.0f;
            gainSmooth_ += (target - gainSmooth_) * 0.05f;
            buf[i] = in * gainSmooth_;
        }
    }

private:
    float AttackReleaseCoeff(float ms) const {
        if (ms <= 0.0f) return 1.0f;
        return 1.0f - std::exp(-1.0f / (0.001f * ms * sampleRate_));
    }

    float sampleRate_;
    float envelope_ = 0.0f;
    bool isOpen_ = false;
    float gainSmooth_ = 0.0f;
};
