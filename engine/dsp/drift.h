#pragma once
// drift.h : クロックドリフト補正 = PI コントローラ + 可変比リサンプラ(ASRC)
//
// 理屈(実装ガイド §4.3):
//   入力デバイス A と出力デバイス B は別々の水晶で動くため、名目 48kHz でも
//   実周波数が数十 ppm ずれる。リングの充填率を 50% に保つように
//   リサンプル比を微調整し続けることで、ずれを永続的に吸収する。
//
// libsamplerate の src_ratio は「出力レート / 入力レート」であることに注意。
//   充填率が高い(入力側が速い) → 入力を多めに消費したい
//   → 出力/入力 比は 1 より小さくする(ratio = 1 / drift)
//
// チャンネル数について: エンジン内部フォーマットは float32 / プレーナ
// (実装ガイド §2.4)。つまり 1 チャンネル = 1 SpscRing<float> であり、
// AsrcReader もモノラル(1 チャンネル 1 インスタンス)。複数チャンネルを
// 同期させたい場合は、同じ DriftController が出す srcRatio を全チャンネル
// 分の AsrcReader に共通で渡す(呼び出し側=エンジン境界の責務)。

#include <algorithm>
#include <samplerate.h>
#include <stdexcept>
#include <vector>

#include "device/iaudio_device.h"  // Lane
#include "rt/spsc_ring.h"

// --- PI コントローラ ------------------------------------------------------
class DriftController {
public:
    // fillRatio: リング充填率 0..1(呼び出し側で平滑化済みを渡すこと)
    // 戻り値: 入力クロック補正比 drift(1.0 付近)。
    //         src_ratio には 1.0/drift を渡す。
    double Update(double fillRatio) {
        const double err = fillRatio - 0.5;
        integ_ = std::clamp(integ_ + err * ki_, -maxCorr_, maxCorr_);
        const double corr = std::clamp(err * kp_ + integ_, -maxCorr_, maxCorr_);
        last_ = 1.0 + corr;
        return last_;
    }
    double Last() const { return last_; }

    // チューニング指針:
    //  - ピッチ揺れ(ワウ)が聴こえる → kp/ki を下げる
    //  - 収束前に xrun する → リング容量を増やす(ゲインを上げるより先に)
    void SetGains(double kp, double ki) { kp_ = kp; ki_ = ki; }

private:
    double kp_ = 1e-4;
    double ki_ = 1e-7;
    double maxCorr_ = 5e-4;  // ±500ppm(通常のデバイス間ドリフトは ±100ppm 以内)
    double integ_ = 0.0;
    double last_ = 1.0;
};

// --- 充填率の平滑化(EMA) -------------------------------------------------
class Ema {
public:
    explicit Ema(double alpha = 0.99) : alpha_(alpha) {}
    double Push(double v) {
        if (!primed_) { y_ = v; primed_ = true; }
        else y_ = alpha_ * y_ + (1.0 - alpha_) * v;
        return y_;
    }
    double Value() const { return y_; }
private:
    double alpha_; double y_ = 0.5; bool primed_ = false;
};

// --- ASRC 付きリング読み出し(モノラル) -------------------------------------
// 消費者(出力デバイス側)から使う。1 チャンネル分の SpscRing<float> を
// 直接読む。ステレオ/マルチチャンネルは、チャンネル数だけ AsrcReader を
// 用意し、同じ srcRatio を渡して呼び出すことで同期させる。
//
// RT 安全性メモ: libsamplerate はハンドル生成(src_new)時にのみ確保を行い、
// src_process 内ではアロケーションしない。ハンドルは必ず起動前に作ること。
class AsrcReader {
public:
    // lane: 実装ガイド §4.3.2/付録A「64 サンプルでは SRC_SINC_MEDIUM_QUALITY
    // は重い。RT Lane は SRC_SINC_FASTEST、Compat Lane は品質を上げる」。
    // 既定は Lane::Compat(既存呼び出し側の動作を変えないため)。
    AsrcReader(SpscRing<float>& ring, int maxOutFrames, Lane lane = Lane::Compat)
        : ring_(ring), stage_((size_t)maxOutFrames * 4) {
        int err = 0;
        const int quality = (lane == Lane::RT) ? SRC_SINC_FASTEST : SRC_SINC_MEDIUM_QUALITY;
        state_ = src_new(quality, /*channels=*/1, &err);
        if (!state_) throw std::runtime_error("src_new failed");
    }
    ~AsrcReader() { if (state_) src_delete(state_); }
    AsrcReader(const AsrcReader&) = delete;
    AsrcReader& operator=(const AsrcReader&) = delete;

    // ムーブ構築のみ許可する(ring_ が参照メンバのためムーブ代入は不可)。
    // std::vector<StripRuntime> の emplace_back による再確保(既存要素を
    // 新しいバッファへ移す)で必要になる。
    AsrcReader(AsrcReader&& other) noexcept
        : ring_(other.ring_), stage_(std::move(other.stage_)),
          stagePos_(other.stagePos_), stageLen_(other.stageLen_),
          state_(other.state_) {
        other.state_ = nullptr;
    }
    AsrcReader& operator=(AsrcReader&&) = delete;

    // outFrames フレームを必ず埋めて返す(足りない分は無音)。RT から呼ぶ。
    // 戻り値: このブロックでアンダーランが発生したか
    bool Read(float* out, int outFrames, double srcRatio) {
        bool underrun = false;
        int produced = 0;
        while (produced < outFrames) {
            // ステージが空なら、リングから補充する
            if (stagePos_ >= stageLen_) {
                const size_t want = stage_.size();
                const size_t got = ring_.Read(stage_.data(), want);
                if (got == 0) {                     // 実質空
                    underrun = true;
                    // 無音で埋めて即帰る(充填率リセットは呼び出し側で)
                    std::fill(out + produced, out + outFrames, 0.0f);
                    return underrun;
                }
                stageLen_ = got;
                stagePos_ = 0;
            }
            SRC_DATA d{};
            d.data_in       = stage_.data() + stagePos_;
            d.input_frames  = (long)(stageLen_ - stagePos_);
            d.data_out      = out + produced;
            d.output_frames = outFrames - produced;
            d.src_ratio     = srcRatio;
            d.end_of_input  = 0;
            if (src_process(state_, &d) != 0) { underrun = true; break; }
            stagePos_ += (size_t)d.input_frames_used;
            produced  += (int)d.output_frames_gen;
            if (d.input_frames_used == 0 && d.output_frames_gen == 0) break; // 保険
        }
        if (produced < outFrames) {
            std::fill(out + produced, out + outFrames, 0.0f);
            underrun = true;
        }
        return underrun;
    }

private:
    SpscRing<float>& ring_;
    std::vector<float> stage_;   // 起動前に確保済み。RT 中は伸びない
    size_t stagePos_ = 0, stageLen_ = 0;
    SRC_STATE* state_ = nullptr;
};
