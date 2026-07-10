#pragma once
// meter.h : ピーク/RMS メータリング(実装ガイド §5.5)
// RT はピーク/RMS を atomic<float> に書くだけ。UI は任意のレート
// (実装ガイドの想定は 30fps 程度)でポーリングして読む。

#include <atomic>
#include <cmath>

class Meter {
public:
    Meter() = default;

    // std::atomic はデフォルトではムーブ不可のため明示的に定義する
    // (param_buffer.h の TripleBuffer と同じ理由。StripRuntime/BusRuntime が
    // std::vector<...>::emplace_back で再確保される際に必要)。
    Meter(Meter&& other) noexcept
        : peak_(other.peak_.load(std::memory_order_relaxed)),
          rms_(other.rms_.load(std::memory_order_relaxed)) {}
    Meter& operator=(Meter&&) = delete;
    Meter(const Meter&) = delete;
    Meter& operator=(const Meter&) = delete;

    void ProcessBlock(const float* buf, int frames) {
        float peak = 0.0f;
        double sumSq = 0.0;
        for (int i = 0; i < frames; ++i) {
            const float a = std::fabs(buf[i]);
            if (a > peak) peak = a;
            sumSq += (double)buf[i] * (double)buf[i];
        }
        const float rms = frames > 0 ? (float)std::sqrt(sumSq / frames) : 0.0f;
        peak_.store(peak, std::memory_order_relaxed);
        rms_.store(rms, std::memory_order_relaxed);
    }

    float PeakLinear() const { return peak_.load(std::memory_order_relaxed); }
    float RmsLinear() const { return rms_.load(std::memory_order_relaxed); }

private:
    std::atomic<float> peak_{0.0f};
    std::atomic<float> rms_{0.0f};
};
