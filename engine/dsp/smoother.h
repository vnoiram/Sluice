#pragma once
// smoother.h : ブロック単位の一極平滑化係数(実装ガイド §5.5 が挙げる
// 「係数切替時はクロスフェードか係数スムージング」への対応の共通部品)。
//
// compressor.h の AttackReleaseCoeff は「毎サンプル」1回だけ適用する前提の
// per-sample 係数だが、ここでの用途(EQ 係数のスムージング等)は
// SetParams() 呼び出し時に決まった目標値へ、以後のブロック処理のたびに
// 少しずつ近づけていく「ブロック単位」の更新になる。frames サンプルぶんの
// 経過時間を考慮した係数にすることで、per-sample の一極フィルタを frames
// 回連続で適用したのと厳密に同じ結果になる(target が一定の間は
// (1 - perSampleCoeff)^frames == exp(-frames / (0.001*ms*sampleRate)) が
// 成り立つため)。

#include <cmath>

// current += (target - current) * SmoothingCoeff(...) という形で、
// current を target へブロック単位で近づけるための係数を返す。
// ms <= 0 の場合は即座に target へスナップする(スムージング無効)。
inline float SmoothingCoeff(float ms, int frames, float sampleRate) {
    if (ms <= 0.0f) return 1.0f;
    return 1.0f - std::exp(-static_cast<float>(frames) / (0.001f * ms * sampleRate));
}
