#pragma once
// delay_line.h : 固定長リングバッファによるモノラル遅延線
// (limiter.h/compressor.h の lookahead 対応で使う共通部品、実装ガイド §5.5)。
//
// 構築時に1回だけ確保する(RT安全: engine/rt/rt_alloc_guard.h の制約により
// RTコールバック内では operator new を呼べないため、必ず制御スレッド側
// —— LimiterRuntime/CompressorRuntime のコンストラクタが呼ばれる場所 ——
// で構築すること)。

#include <vector>

class DelayLine {
public:
    // maxOffsetFrames: Push() の offsetFrames に渡せる最大値。内部では
    // maxOffsetFrames+1 サンプルぶんのリングを確保する(offsetFrames ==
    // maxOffsetFrames のときに「ちょうど1周前」を正しく指すには、直近の
    // 書き込みと衝突しないよう1サンプルぶんの余白が必要なため)。
    explicit DelayLine(int maxOffsetFrames)
        : buf_((size_t)(maxOffsetFrames > 0 ? maxOffsetFrames : 0) + 1, 0.0f) {}

    int MaxOffsetFrames() const { return (int)buf_.size() - 1; }

    // x を書き込み、offsetFrames(0 〜 MaxOffsetFrames())サンプル前に
    // 書き込まれた値を返す。offsetFrames == 0 なら x をそのまま返す
    // (遅延なし)。
    float Push(float x, int offsetFrames) {
        buf_[writePos_] = x;
        const size_t cap = buf_.size();
        const size_t readPos = (writePos_ + cap - (size_t)offsetFrames) % cap;
        const float delayed = buf_[readPos];
        writePos_ = (writePos_ + 1) % cap;
        return delayed;
    }

private:
    std::vector<float> buf_;
    size_t writePos_ = 0;
};
