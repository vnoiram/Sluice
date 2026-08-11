#pragma once
// strip.h : 入力ストリップ(実装ガイド §5.4)
//
// 1 StripRuntime = 1 モノラルチャンネル分の入力パス。ステレオ入力は
// 2 つの StripRuntime(例: "Mic L"/"Mic R")として扱う(ステレオ連結/
// リンク操作は今後の課題。ミニマム実装の意図的な割り切り)。
//
// 処理順(実装ガイド §5.4.1): ASRC(クロックドリフト吸収)→ gain(ミュート/
// ソロ込み、ブロック内線形補間)→ gate → EQ → comp → メータリング。

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

#include "dsp/compressor.h"
#include "dsp/drift.h"
#include "dsp/eq.h"
#include "dsp/gate.h"
#include "dsp/meter.h"
#include "graph/gain_util.h"
#include "graph/param_buffer.h"

// UI から更新されるパラメータ(トリプルバッファでスナップショットされる)。
// ヒープ確保を伴うメンバを持たないこと(RT 側のコピーが RT 安全である
// ため)。routingGain は本来 std::vector(実装ガイド §5.4)だが、
// コピー時のアロケーションを避けるため固定長 std::array にしている。
struct StripParams {
    static constexpr int kMaxBuses = 64;
    static constexpr float kRoutingGainMuted = -1000.0f;  // 実質 -inf(送らない)

    float gainDb = 0.0f;
    bool mute = false;
    bool solo = false;
    GateParams gate;
    EqParams eq;
    CompParams comp;
    std::array<float, kMaxBuses> routingGain{};  // routingGain[busIndex] = センドゲイン(dB)

    StripParams() { routingGain.fill(kRoutingGainMuted); }
};

class StripRuntime {
public:
    // inputRing: このストリップが読む物理/仮想入力のチャンネル(プレーナ)。
    // maxBlockFrames: マスターの最大ブロックフレーム数(起動前に scratch を確保する)。
    // lane: このストリップが属するデバイスのレーン。ASRC の品質選択に使う
    // (実装ガイド §4.3.2、engine/dsp/drift.h の AsrcReader 参照)。既定は
    // Lane::Compat(呼び出し側を変えない既存テストとの後方互換のため)。
    StripRuntime(SpscRing<float>& inputRing, int maxBlockFrames, float sampleRate,
                const StripParams& initial, int boundaryIndex, Lane lane = Lane::Compat)
        : asrc_(inputRing, maxBlockFrames, lane),
          outBuf_((size_t)maxBlockFrames, 0.0f),
          params_(initial),
          gate_(sampleRate),
          eq_(sampleRate),
          comp_(sampleRate),
          boundaryIndex_(boundaryIndex) {
        lastRoutingGain_ = initial.routingGain;
    }

    // ブロック先頭で呼ぶ。ASRC 経由で frames フレーム取得し、
    // gain → gate → EQ → comp を適用する(実装ガイド §5.4.1)。
    // 戻り値: このブロックでアンダーランが発生したか(呼び出し側は該当
    // InputBoundary の再プリフィルに使う)。
    bool Process(int frames, double srcRatio, bool anySolo) {
        const StripParams p = params_.Current();
        lastRoutingGain_ = p.routingGain;

        bool underrun = asrc_.Read(outBuf_.data(), frames, srcRatio);
        ApplyChain(p, frames, anySolo);
        return underrun;
    }

    // InputBoundary がまだプリフィル待ちのときに呼ぶ(無音を出す)。
    void ProcessSilence(int frames, bool anySolo) {
        const StripParams p = params_.Current();
        lastRoutingGain_ = p.routingGain;
        std::fill(outBuf_.begin(), outBuf_.begin() + frames, 0.0f);
        ApplyChain(p, frames, anySolo);
    }

    void PublishParams(const StripParams& p) { params_.Publish(p); }
    bool IsSoloed() const { return params_.Current().solo; }

    const float* Output() const { return outBuf_.data(); }

    float RoutingGainLinear(int busIndex) const {
        float db = lastRoutingGain_[(size_t)busIndex];
        if (db <= StripParams::kRoutingGainMuted) return 0.0f;
        return DbToLinear(db);
    }

    int BoundaryIndex() const { return boundaryIndex_; }
    float PeakLinear() const { return meter_.PeakLinear(); }
    float RmsLinear() const { return meter_.RmsLinear(); }

private:
    void ApplyChain(const StripParams& p, int frames, bool anySolo) {
        ApplyGain(p, frames, anySolo);
        gate_.ProcessBlock(outBuf_.data(), frames, p.gate);
        eq_.SetParams(p.eq);
        eq_.ProcessBlock(outBuf_.data(), frames);
        comp_.ProcessBlock(outBuf_.data(), frames, p.comp);
        meter_.ProcessBlock(outBuf_.data(), frames);
    }

    void ApplyGain(const StripParams& p, int frames, bool anySolo) {
        const bool audible = !p.mute && (!anySolo || p.solo);
        const float target = audible ? DbToLinear(p.gainDb) : 0.0f;
        // ブロック内で smoothedGainLin_ → target へ線形補間しながら適用
        // (実装ガイド §5.4.2: ゲイン変化はブロック内で線形補間、ザッピング
        // ノイズ防止)。
        for (int i = 0; i < frames; ++i) {
            const float t = (float)(i + 1) / (float)frames;
            const float g = smoothedGainLin_ + (target - smoothedGainLin_) * t;
            outBuf_[(size_t)i] *= g;
        }
        smoothedGainLin_ = target;
    }

    AsrcReader asrc_;
    std::vector<float> outBuf_;
    TripleBuffer<StripParams> params_;
    GateRuntime gate_;
    EqRuntime eq_;
    CompressorRuntime comp_;
    Meter meter_;
    std::array<float, StripParams::kMaxBuses> lastRoutingGain_{};
    float smoothedGainLin_ = 1.0f;
    int boundaryIndex_;
};
