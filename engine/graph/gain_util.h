#pragma once
// gain_util.h : dB ⇔ 線形ゲイン変換(strip.h / bus.h 共通)

#include <cmath>

inline float DbToLinear(float db) {
    return std::pow(10.0f, db / 20.0f);
}
