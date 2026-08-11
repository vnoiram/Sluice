#pragma once
// bus.h : 出力バス(実装ガイド §5.4 / §5.4.1)
//
// 各ストリップの出力を routingGain で重み付けして N×M ミックスし、
// バスゲイン → リミッタ(実装ガイド §5.5)を適用してから、1 つ以上の
// 出力リング(物理/仮想出力のチャンネル)へ書き込む。

#include <algorithm>
#include <vector>

#include "dsp/limiter.h"
#include "dsp/meter.h"
#include "graph/gain_util.h"
#include "graph/param_buffer.h"
#include "graph/simd_mix.h"
#include "graph/strip.h"
#include "rt/spsc_ring.h"

struct BusParams {
    float gainDb = 0.0f;
    LimiterParams limiter;
};

class BusRuntime {
public:
    // outputs: このバスの割当先(物理/仮想出力のチャンネル。複数可)。
    // ポインタの寿命は EngineGraph 側が保証する(デバイスが所有する
    // RenderRing を指すだけで、BusRuntime は所有しない)。
    BusRuntime(int maxBlockFrames, std::vector<SpscRing<float>*> outputs,
              const BusParams& initial)
        : mixBuf_((size_t)maxBlockFrames, 0.0f),
          outputs_(std::move(outputs)),
          params_(initial) {}

    void PublishParams(const BusParams& p) { params_.Publish(p); }
    float PeakLinear() const { return meter_.PeakLinear(); }
    float RmsLinear() const { return meter_.RmsLinear(); }

    // 全ストリップを busIndex 番目のセンドゲインで重み付けしてミックスし、
    // バスゲイン→リミッタを適用してから出力リングへ書き込む
    // (実装ガイド §5.4.1 の N×M ミックスループ + §5.5 のバス用リミッタ)。
    // 内側のループ(dst[i] += src[i] * gain)は graph/simd_mix.h の
    // MixAddScaled に切り出しており、AVX2 でビルドされた場合(既定では
    // sluice-engine の Release 構成のみ)は 8 サンプル単位で処理される。
    void MixAndWrite(const std::vector<StripRuntime>& strips, int busIndex, int frames) {
        std::fill(mixBuf_.begin(), mixBuf_.begin() + frames, 0.0f);
        for (const auto& s : strips) {
            const float g = s.RoutingGainLinear(busIndex);
            if (g <= 0.0f) continue;
            MixAddScaled(mixBuf_.data(), s.Output(), g, frames);
        }

        const BusParams p = params_.Current();
        const float busGain = DbToLinear(p.gainDb);
        for (int i = 0; i < frames; ++i) mixBuf_[(size_t)i] *= busGain;

        limiter_.ProcessBlock(mixBuf_.data(), frames, p.limiter);
        meter_.ProcessBlock(mixBuf_.data(), frames);

        for (auto* ring : outputs_) {
            if (ring) ring->Write(mixBuf_.data(), (size_t)frames);
        }
    }

private:
    std::vector<float> mixBuf_;
    std::vector<SpscRing<float>*> outputs_;
    TripleBuffer<BusParams> params_;
    LimiterRuntime limiter_;
    Meter meter_;
};
