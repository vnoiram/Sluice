#pragma once
// simd_mix.h : N×M ミックスループの内側(dst[i] += src[i] * gain)の AVX2
// 実装(実装ガイド §5.4.1「N×M のミックスループが最大の計算量...最初から
// AVX2(_mm256_fmadd_ps)で 8 サンプル単位に書く」)。
//
// __AVX2__ が定義されているとき(コンパイラに /arch:AVX2 相当が渡されたとき)
// だけ SIMD 経路を使う。定義されていなければ常にスカラーループにフォール
// バックする。これは意図的な設計判断: engine/tests/ のプラットフォーム
// 非依存コアテスト(test_engine_graph.cpp 等)は ASIO SDK にも Windows API
// にも依存せず「どの環境でもビルドできる」ことが売りなので、AVX2 を
// ビルド要件に加えたくない。engine/CMakeLists.txt では sluice-engine
// ターゲットの Release 構成にのみ /arch:AVX2 を付けており、この場合だけ
// 実際に SIMD 経路が使われる(Debug/テストは常にスカラー経路)。

#include <cstddef>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

// dst[i] += src[i] * gain (i = 0..frames-1)。RT スレッドから呼ばれる想定
// (アロケーションなし、frames は事前に確保済みバッファの範囲内であること
// が呼び出し側の責務)。
inline void MixAddScaled(float* dst, const float* src, float gain, int frames) {
#if defined(__AVX2__)
    const __m256 gainVec = _mm256_set1_ps(gain);
    int i = 0;
    const int simdEnd = frames - (frames % 8);
    for (; i < simdEnd; i += 8) {
        const __m256 s = _mm256_loadu_ps(src + i);
        const __m256 d = _mm256_loadu_ps(dst + i);
        _mm256_storeu_ps(dst + i, _mm256_fmadd_ps(s, gainVec, d));
    }
    for (; i < frames; ++i) dst[i] += src[i] * gain;
#else
    for (int i = 0; i < frames; ++i) dst[i] += src[i] * gain;
#endif
}
