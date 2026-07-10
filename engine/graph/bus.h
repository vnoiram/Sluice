#pragma once
// bus.h : 出力バス(実装ガイド §5.4.1 / §5.4.2)
//
// 各ストリップの出力を routingGain で重み付けして N×M ミックスし、
// バスゲインを掛けて 1 つ以上の出力リング(物理/仮想出力のチャンネル)へ
// 書き込む。リミッタ等(実装ガイド §5.5)は今後のマイルストーンで追加する
// (今はまだ無い)。

#include <algorithm>
#include <vector>

#include "graph/gain_util.h"
#include "graph/param_buffer.h"
#include "graph/strip.h"
#include "rt/spsc_ring.h"

struct BusParams {
    float gainDb = 0.0f;
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

    // 全ストリップを busIndex 番目のセンドゲインで重み付けしてミックスし、
    // 出力リングへ書き込む(実装ガイド §5.4.2 の N×M ミックスループ)。
    void MixAndWrite(const std::vector<StripRuntime>& strips, int busIndex, int frames) {
        std::fill(mixBuf_.begin(), mixBuf_.begin() + frames, 0.0f);
        for (const auto& s : strips) {
            const float g = s.RoutingGainLinear(busIndex);
            if (g <= 0.0f) continue;
            const float* src = s.Output();
            for (int i = 0; i < frames; ++i) mixBuf_[(size_t)i] += src[i] * g;
        }

        const BusParams p = params_.Current();
        const float busGain = DbToLinear(p.gainDb);
        for (int i = 0; i < frames; ++i) mixBuf_[(size_t)i] *= busGain;

        for (auto* ring : outputs_) {
            if (ring) ring->Write(mixBuf_.data(), (size_t)frames);
        }
    }

private:
    std::vector<float> mixBuf_;
    std::vector<SpscRing<float>*> outputs_;
    TripleBuffer<BusParams> params_;
};
