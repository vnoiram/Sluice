#pragma once
// strip.h : 入力ストリップ(実装ガイド §5.4.1)
//
// 1 StripRuntime = 1 モノラルチャンネル分の入力パス。ステレオ入力は
// 2 つの StripRuntime(例: "Mic L"/"Mic R")として扱う(ステレオ連結/
// リンク操作は今後の課題。ミニマム実装の意図的な割り切り)。
//
// 処理順: ASRC(クロックドリフト吸収)→ gain(ミュート/ソロ込み、ブロック内
// 線形補間)。EQ/gate/comp(実装ガイド §5.5)は今後のマイルストーンで
// StripRuntime::Process() に追加する(今はまだ無い)。

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

#include "dsp/drift.h"
#include "graph/gain_util.h"
#include "graph/param_buffer.h"

// UI から更新されるパラメータ(トリプルバッファでスナップショットされる)。
// ヒープ確保を伴うメンバを持たないこと(RT 側のコピーが RT 安全である
// ため)。routingGain は本来 std::vector(実装ガイド §5.4.1)だが、
// コピー時のアロケーションを避けるため固定長 std::array にしている。
struct StripParams {
    static constexpr int kMaxBuses = 64;
    static constexpr float kRoutingGainMuted = -1000.0f;  // 実質 -inf(送らない)

    float gainDb = 0.0f;
    bool mute = false;
    bool solo = false;
    std::array<float, kMaxBuses> routingGain{};  // routingGain[busIndex] = センドゲイン(dB)

    StripParams() { routingGain.fill(kRoutingGainMuted); }
};

class StripRuntime {
public:
    // inputRing: このストリップが読む物理/仮想入力のチャンネル(プレーナ)。
    // maxBlockFrames: マスターの最大ブロックフレーム数(起動前に scratch を確保する)。
    StripRuntime(SpscRing<float>& inputRing, int maxBlockFrames,
                const StripParams& initial, int boundaryIndex)
        : asrc_(inputRing, maxBlockFrames),
          outBuf_((size_t)maxBlockFrames, 0.0f),
          params_(initial),
          boundaryIndex_(boundaryIndex) {
        lastRoutingGain_ = initial.routingGain;
    }

    // ブロック先頭で呼ぶ。ASRC 経由で frames フレーム取得し、gain/mute/solo
    // を適用する。戻り値: このブロックでアンダーランが発生したか
    // (呼び出し側は該当 InputBoundary の再プリフィルに使う)。
    bool Process(int frames, double srcRatio, bool anySolo) {
        const StripParams p = params_.Current();
        lastRoutingGain_ = p.routingGain;

        bool underrun = asrc_.Read(outBuf_.data(), frames, srcRatio);
        ApplyGain(p, frames, anySolo);
        return underrun;
    }

    // InputBoundary がまだプリフィル待ちのときに呼ぶ(無音を出す)。
    void ProcessSilence(int frames, bool anySolo) {
        const StripParams p = params_.Current();
        lastRoutingGain_ = p.routingGain;
        std::fill(outBuf_.begin(), outBuf_.begin() + frames, 0.0f);
        ApplyGain(p, frames, anySolo);
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

private:
    void ApplyGain(const StripParams& p, int frames, bool anySolo) {
        const bool audible = !p.mute && (!anySolo || p.solo);
        const float target = audible ? DbToLinear(p.gainDb) : 0.0f;
        // ブロック内で smoothedGainLin_ → target へ線形補間しながら適用
        // (実装ガイド §5.4.3: ゲイン変化はブロック内で線形補間、ザッピング
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
    std::array<float, StripParams::kMaxBuses> lastRoutingGain_{};
    float smoothedGainLin_ = 1.0f;
    int boundaryIndex_;
};
