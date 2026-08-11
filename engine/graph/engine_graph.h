#pragma once
// engine_graph.h : エンジングラフ本体(実装ガイド §5.4)
//
// InputBoundary: 実装ガイド §5.1 の「ASRC はデバイス側でなくエンジン
// 境界に置く」を体現する部品。1 つの入力デバイスに対応し、その代表
// チャンネルの充填率からドリフト補正比を計算して、同じデバイスの全
// チャンネル(= 複数の StripRuntime)で共有する(main.cpp の Phase 0
// パススルーで書いていたロジックの一般化)。
//
// EngineGraph: 1 つのトポロジ(ストリップ/バス構成)。トポロジ変更
// (ストリップ追加等)は丸ごと作り直す(実装ガイド §5.4.3)。
//
// GraphHandle: RCU 方式のグラフ差し替え。
//   1. 制御スレッドが新しい EngineGraph を構築
//   2. Publish() で std::atomic<EngineGraph*> を差し替え
//   3. RT スレッドが次のブロックで新しいポインタを Acquire したのを
//      確認してから、制御スレッドが旧グラフを破棄する
//   (世代カウンタの代わりに「RT が最後に Acquire したポインタ」を見る
//   簡易版。挙動は同じ: RT が旧グラフを使い終えてから解放する。)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "dsp/drift.h"
#include "graph/bus.h"
#include "graph/strip.h"
#include "rt/spsc_ring.h"

class InputBoundary {
public:
    explicit InputBoundary(SpscRing<float>& referenceRing) : ring_(referenceRing) {}

    // ブロック先頭で 1 回呼ぶ。true を返せば *outRatio が有効(通常処理)。
    // false ならまだプリフィル待ち(このバウンダリに属する全チャンネルは
    // 無音を出す)。
    bool UpdateAndGetRatio(double* outRatio) {
        if (!prefilled_) {
            if (ring_.FillRatio() < 0.5) return false;
            prefilled_ = true;
        }
        const double fill = fillEma_.Push(ring_.FillRatio());
        const double driftRatio = drift_.Update(fill);
        *outRatio = 1.0 / driftRatio;
        return true;
    }

    // 所属するいずれかのチャンネルでアンダーランが起きたら呼ぶ
    // (再プリフィルへ戻す)。
    void NotifyUnderrun() { prefilled_ = false; }

private:
    SpscRing<float>& ring_;
    DriftController drift_;
    Ema fillEma_{0.99};
    bool prefilled_ = false;
};

class EngineGraph {
public:
    EngineGraph(std::vector<InputBoundary> boundaries, std::vector<StripRuntime> strips,
               std::vector<BusRuntime> buses)
        : boundaries_(std::move(boundaries)),
          strips_(std::move(strips)),
          buses_(std::move(buses)),
          boundaryRatio_(boundaries_.size(), 1.0),
          boundaryReady_(boundaries_.size(), false),
          boundaryUnderrun_(boundaries_.size(), false) {}

    // マスターコールバックから毎ブロック呼ぶ(実装ガイド §5.4.1)。
    void Process(int frames) {
        for (size_t i = 0; i < boundaries_.size(); ++i)
            boundaryReady_[i] = boundaries_[i].UpdateAndGetRatio(&boundaryRatio_[i]);
        std::fill(boundaryUnderrun_.begin(), boundaryUnderrun_.end(), false);

        bool anySolo = false;
        for (const auto& s : strips_) {
            if (s.IsSoloed()) { anySolo = true; break; }
        }

        for (auto& s : strips_) {
            const int bi = s.BoundaryIndex();
            if (boundaryReady_[(size_t)bi]) {
                if (s.Process(frames, boundaryRatio_[(size_t)bi], anySolo))
                    boundaryUnderrun_[(size_t)bi] = true;
            } else {
                s.ProcessSilence(frames, anySolo);
            }
        }

        for (size_t bi = 0; bi < boundaries_.size(); ++bi)
            if (boundaryUnderrun_[bi]) boundaries_[bi].NotifyUnderrun();

        for (size_t bi = 0; bi < buses_.size(); ++bi)
            buses_[bi].MixAndWrite(strips_, (int)bi, frames);
    }

    StripRuntime& Strip(size_t i) { return strips_[i]; }
    BusRuntime& Bus(size_t i) { return buses_[i]; }
    size_t StripCount() const { return strips_.size(); }
    size_t BusCount() const { return buses_.size(); }

private:
    std::vector<InputBoundary> boundaries_;
    std::vector<StripRuntime> strips_;
    std::vector<BusRuntime> buses_;
    // Process() 内のワークバッファ。構築時に確保し、要素数を変えず
    // 上書きするだけ(RT 中に伸びない)。
    std::vector<double> boundaryRatio_;
    std::vector<bool> boundaryReady_;
    std::vector<bool> boundaryUnderrun_;
};

class GraphHandle {
public:
    ~GraphHandle() { delete current_.load(std::memory_order_relaxed); }

    // RT スレッド専用。
    EngineGraph* Acquire() {
        EngineGraph* g = current_.load(std::memory_order_acquire);
        lastUsed_.store(g, std::memory_order_release);
        return g;
    }

    // 制御スレッド専用(単一ライタ前提)。RT が旧グラフの使用を終えた
    // (= 次のブロックで新しいポインタを Acquire した)のを確認してから
    // 旧グラフを破棄する。
    //
    // 既知の制限: RT スレッドが全く動いていない(デバイス未 Start)間に
    // Publish() を呼ぶと、RT が Acquire() するまで待ち続ける。今の
    // ミニマム実装では「エンジン稼働中にだけ Publish する」運用を前提と
    // している。
    void Publish(std::unique_ptr<EngineGraph> newGraph) {
        EngineGraph* old = current_.exchange(newGraph.release(), std::memory_order_acq_rel);
        if (!old) return;
        while (lastUsed_.load(std::memory_order_acquire) == old) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        delete old;
    }

private:
    std::atomic<EngineGraph*> current_{nullptr};
    std::atomic<EngineGraph*> lastUsed_{nullptr};
};
