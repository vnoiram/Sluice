#pragma once
// param_buffer.h : 制御スレッド → RT スレッドのパラメータ受け渡し
// (実装ガイド §5.4.2「トリプルバッファ」)
//
// 制御スレッド(単一)は Publish() で好きなときに新しい値を渡し、
// RT スレッド(単一)は Current() で最新値を「コピーとして」取得する。
// 参照を保持しない設計にすることで、Publish() との競合窓を「1回の
// コピー時間」まで縮める。3 スロットを直前に書いたスロットを避けて
// ローテーションすることで、RT 側がコピー中に同じスロットへ書き込まれる
// 実用上のリスクを避ける(スロットが再利用されるのは 2 回先の Publish
// から)。
//
// 前提: T はコピー可能な POD 的な構造体(ヒープ確保を伴うメンバを
// 持たないこと。RT 側のコピーがアロケーションしないようにするため)。

#include <array>
#include <atomic>

template <typename T>
class TripleBuffer {
public:
    explicit TripleBuffer(const T& initial) : slots_{initial, initial, initial} {
        current_.store(0, std::memory_order_relaxed);
    }

    // std::atomic はデフォルトではムーブ不可のため明示的に定義する。
    // StripRuntime/BusRuntime が std::vector<...>::emplace_back で
    // 再確保(既存要素を新しいバッファへ移す)される際に必要。
    // 使うのはグラフ構築中(単一スレッド)のみを想定しているため、
    // atomic の値をそのまま load して新しい atomic を初期化するだけでよい。
    TripleBuffer(TripleBuffer&& other) noexcept
        : slots_(std::move(other.slots_)),
          current_(other.current_.load(std::memory_order_relaxed)),
          lastPublished_(other.lastPublished_) {}
    TripleBuffer& operator=(TripleBuffer&&) = delete;
    TripleBuffer(const TripleBuffer&) = delete;
    TripleBuffer& operator=(const TripleBuffer&) = delete;

    // 制御スレッド専用(単一ライタ前提。複数スレッドから呼ばない)。
    void Publish(const T& value) {
        int next = (lastPublished_ + 1) % 3;
        slots_[(size_t)next] = value;
        current_.store(next, std::memory_order_release);
        lastPublished_ = next;
    }

    // RT スレッド専用。値のコピーを返す(ヒープ確保なし。T が POD 的で
    // あることが前提)。
    T Current() const {
        return slots_[(size_t)current_.load(std::memory_order_acquire)];
    }

private:
    std::array<T, 3> slots_;
    std::atomic<int> current_;
    int lastPublished_ = 0;
};
