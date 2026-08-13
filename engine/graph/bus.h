#pragma once
// bus.h : 出力バス(実装ガイド §5.4 / §5.4.1)
//
// 各ストリップの出力を routingGain で重み付けして N×M ミックスし、
// バスゲイン → リミッタ(実装ガイド §5.5)を適用してから、1 つ以上の
// 出力リング(物理/仮想出力のチャンネル)へ書き込む。

#include <algorithm>
#include <vector>

#include "dsp/drift.h"
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
    // sampleRate: limiter_(lookahead 遅延線のサイズ決定)に使う。
    // outputBoundaryIndex: このバスが非マスター出力デバイスに属する場合、
    // 対応する OutputBoundary のインデックス(engine_graph.h)。既定の -1 は
    // 「マスターバス、ASRC を適用しない」を意味する(実装ガイド §5.1 の
    // 考え方どおり、マスター自身の callback がブロック境界そのものなので
    // 不要かつ有害)。>= 0 のときだけ outputs_ の非 null 要素ごとに
    // AsrcWriter を確保する。
    BusRuntime(int maxBlockFrames, float sampleRate, std::vector<SpscRing<float>*> outputs,
              const BusParams& initial, int outputBoundaryIndex = -1,
              Lane lane = Lane::Compat)
        : mixBuf_((size_t)maxBlockFrames, 0.0f),
          outputs_(std::move(outputs)),
          params_(initial),
          limiter_(sampleRate),
          outputBoundaryIndex_(outputBoundaryIndex) {
        if (outputBoundaryIndex_ >= 0) {
            asrcWriters_.reserve(outputs_.size());
            for (auto* r : outputs_)
                if (r) asrcWriters_.emplace_back(*r, maxBlockFrames, lane);
        }
    }

    void PublishParams(const BusParams& p) { params_.Publish(p); }
    float PeakLinear() const { return meter_.PeakLinear(); }
    float RmsLinear() const { return meter_.RmsLinear(); }
    int OutputBoundaryIndex() const { return outputBoundaryIndex_; }

    // 直近の MixAndWrite でオーバーラン(非マスター出力側の ASRC 書き込み
    // 失敗)が起きたかを取得し、フラグをクリアする。呼び出し側(EngineGraph)
    // は対応する OutputBoundary::NotifyOverrun() へ橋渡しする。
    bool ConsumeOverrun() {
        const bool o = lastOverrun_;
        lastOverrun_ = false;
        return o;
    }

    // 全ストリップを busIndex 番目のセンドゲインで重み付けしてミックスし、
    // バスゲイン→リミッタを適用してから出力リングへ書き込む
    // (実装ガイド §5.4.1 の N×M ミックスループ + §5.5 のバス用リミッタ)。
    // 内側のループ(dst[i] += src[i] * gain)は graph/simd_mix.h の
    // MixAddScaled に切り出しており、AVX2 でビルドされた場合(既定では
    // sluice-engine の Release 構成のみ)は 8 サンプル単位で処理される。
    //
    // srcRatio: 非マスター出力側の ASRC 比(outputBoundaryIndex < 0 の
    // マスターバスでは無視される)。
    void MixAndWrite(const std::vector<StripRuntime>& strips, int busIndex, int frames,
                     double srcRatio = 1.0) {
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

        if (asrcWriters_.empty()) {
            for (auto* ring : outputs_) {
                if (ring) ring->Write(mixBuf_.data(), (size_t)frames);
            }
        } else {
            bool overrun = false;
            for (auto& w : asrcWriters_)
                if (w.Write(mixBuf_.data(), frames, srcRatio)) overrun = true;
            lastOverrun_ = overrun;
        }
    }

private:
    std::vector<float> mixBuf_;
    std::vector<SpscRing<float>*> outputs_;
    std::vector<AsrcWriter> asrcWriters_;  // outputBoundaryIndex_ >= 0 のときのみ非空
    TripleBuffer<BusParams> params_;
    LimiterRuntime limiter_;
    Meter meter_;
    int outputBoundaryIndex_ = -1;
    bool lastOverrun_ = false;
};
