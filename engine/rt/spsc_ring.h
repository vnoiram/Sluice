#pragma once
// spsc_ring.h : 単一生産者・単一消費者ロックフリーリングバッファ
//
// 使い方の前提:
//   - Write() を呼ぶスレッドは常に 1 本(デバイス A の RT スレッド)
//   - Read() を呼ぶスレッドは常に 1 本(デバイス B の RT スレッド)
//   この前提が守られる限り、mutex なしで正しく動く。
//
// メモリオーダの意味(初学者向け):
//   - 生産者は「データを書いた後に」writePos を release で公開する
//   - 消費者は writePos を acquire で読むことで、その位置までの
//     データ書き込みが見えることが保証される(逆方向も同様)

#include <atomic>
#include <cassert>
#include <cstddef>
#include <vector>

template <typename T>
class SpscRing {
public:
    explicit SpscRing(size_t capacityPow2)
        : buf_(capacityPow2), mask_(capacityPow2 - 1) {
        assert((capacityPow2 & mask_) == 0 && "capacity must be power of 2");
    }

    // 生産者スレッド専用。書けた個数を返す(< n ならオーバーラン気味)
    size_t Write(const T* data, size_t n) {
        const size_t w = writePos_.load(std::memory_order_relaxed);
        const size_t r = readPos_.load(std::memory_order_acquire);
        const size_t freeCount = buf_.size() - (w - r);
        if (n > freeCount) n = freeCount;
        for (size_t i = 0; i < n; ++i) buf_[(w + i) & mask_] = data[i];
        writePos_.store(w + n, std::memory_order_release);
        return n;
    }

    // 消費者スレッド専用。読めた個数を返す(< n ならアンダーラン気味)
    size_t Read(T* out, size_t n) {
        const size_t r = readPos_.load(std::memory_order_relaxed);
        const size_t w = writePos_.load(std::memory_order_acquire);
        const size_t avail = w - r;
        if (n > avail) n = avail;
        for (size_t i = 0; i < n; ++i) out[i] = buf_[(r + i) & mask_];
        readPos_.store(r + n, std::memory_order_release);
        return n;
    }

    // 消費者スレッド専用。プリフィル用: n 個読み捨て/確認せず進める用途はここでは不要
    size_t Size() const {
        return writePos_.load(std::memory_order_acquire)
             - readPos_.load(std::memory_order_acquire);
    }
    size_t Capacity() const { return buf_.size(); }

    double FillRatio() const {
        return static_cast<double>(Size()) / static_cast<double>(buf_.size());
    }

private:
    std::vector<T> buf_;
    size_t mask_;
    // 64 バイト境界に分離し、2 スレッドが同一キャッシュラインを
    // 取り合う false sharing を防ぐ
    alignas(64) std::atomic<size_t> writePos_{0};
    alignas(64) std::atomic<size_t> readPos_{0};
};
