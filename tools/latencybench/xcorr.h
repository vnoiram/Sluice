#pragma once
// xcorr.h : 信号生成 + 相互相関による往復レイテンシ測定の純粋ロジック
//           (実装ガイド §7.3「検証ツール tools/latencybench」)
//
// Windows API に一切依存しない(cstdint/vector/cmath のみ)。仮想デバイスの
// 再生側へ既知信号(インパルス、または M 系列)を出力し、同じケーブルの
// 録音側から取得した信号との相互相関でピーク位置を求め、往復サンプル数を
// 数える(実装ガイド §7.3「測定原理」)。この純粋ロジック部分をここに
// 切り出すことで、実デバイス I/O を伴わないオフライン単体テストを可能に
// している(tools/latencybench/tests/test_xcorr.cpp 参照)。短いシーケンス
// 前提で時間領域相互相関を使う(FFT 依存は追加しない、実装ガイド §7.3 の
// 実装方針欄)。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace latencybench {

// 長さ length・振幅 amplitude・位置 impulsePos に単一サンプルのインパルスを
// 置いた信号を生成する(他はすべて無音)。最も単純な測定用信号。
inline std::vector<float> GenerateImpulse(size_t length, size_t impulsePos,
                                           float amplitude = 1.0f) {
    std::vector<float> out(length, 0.0f);
    if (impulsePos < length) out[impulsePos] = amplitude;
    return out;
}

// M 系列(最大長系列)を LFSR で生成する。order(レジスタ長)に応じた
// 原始多項式のタップ位置をテーブルから引く。対応 order: 4〜16。
// 系列長は 2^order - 1、値は ±amplitude の2値(自己相関特性が非常に鋭い
// ため、往復オフセット検出の頑健性がインパルスより高い)。
// 未対応 order の場合は空の vector を返す。
inline std::vector<float> GenerateMls(int order, float amplitude = 1.0f) {
    // タップ位置(1-indexed、原始多項式の非ゼロ項)。よく使われる代表値。
    // 参考: 各種 LFSR 実装で広く使われる標準的なタップの組み合わせ。
    static const std::vector<int> kTaps[17] = {
        /*0*/ {}, /*1*/ {}, /*2*/ {1, 2}, /*3*/ {1, 3}, /*4*/ {1, 4},
        /*5*/ {2, 5}, /*6*/ {1, 6}, /*7*/ {1, 7}, /*8*/ {1, 2, 7, 8},
        /*9*/ {4, 9}, /*10*/ {3, 10}, /*11*/ {9, 11}, /*12*/ {6, 8, 11, 12},
        /*13*/ {9, 10, 12, 13}, /*14*/ {4, 8, 13, 14}, /*15*/ {14, 15},
        /*16*/ {4, 13, 15, 16},
    };
    if (order < 2 || order > 16) return {};

    const int n = order;
    const int length = (1 << n) - 1;
    std::vector<int> reg(n, 1);  // 初期状態: 全 1(ゼロ状態は不可)
    std::vector<float> out;
    out.reserve((size_t)length);

    for (int i = 0; i < length; ++i) {
        out.push_back(reg[n - 1] ? amplitude : -amplitude);
        int fb = 0;
        for (int tap : kTaps[n]) fb ^= reg[tap - 1];
        for (int j = n - 1; j > 0; --j) reg[j] = reg[j - 1];
        reg[0] = fb;
    }
    return out;
}

// reference(再生した既知信号)と recorded(録音結果、reference よりオフセット
// ぶん遅れて + 前後に無音/ノイズが付加されている想定)の相互相関を計算し、
// 最もよく一致するラグ(recorded 側でのオフセット、サンプル数)を返す。
// maxLag: 探索するラグの上限(recorded の長さ - reference の長さ以下にする)。
// 時間領域の総当たり相互相関(短いシーケンス前提、FFT 不使用)。
inline size_t FindOffsetByCrossCorrelation(const std::vector<float>& reference,
                                            const std::vector<float>& recorded,
                                            size_t maxLag) {
    if (reference.empty() || recorded.size() < reference.size()) return 0;
    maxLag = std::min(maxLag, recorded.size() - reference.size());

    size_t bestLag = 0;
    double bestScore = -1.0;  // 相関値は理論上 -energy 以上なので -1 で初期化して十分
    for (size_t lag = 0; lag <= maxLag; ++lag) {
        double score = 0.0;
        for (size_t i = 0; i < reference.size(); ++i)
            score += (double)reference[i] * (double)recorded[lag + i];
        if (score > bestScore) {
            bestScore = score;
            bestLag = lag;
        }
    }
    return bestLag;
}

}  // namespace latencybench
