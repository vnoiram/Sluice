#pragma once
// wasapi_period.h : WASAPI 共有モード周期選択の純粋ロジック(実装ガイド §5.2.1)
//
// Windows API に一切依存しない(cstdint のみ)。ChoosePeriodFrames は
// wasapi_device.cpp の Open()/Probe() 双方から呼ばれる共通ロジックであり、
// ここに切り出すことでオフラインでの単体テスト(WIN32 ガード不要)を
// 可能にしている(engine/tests/test_wasapi_period_selection.cpp 参照)。

#include <cstdint>

namespace wasapi {

// 要求周期 want を [min, max] の範囲に収め、fundamental の倍数に丸める。
// 実装ガイド §5.2.1: 「fundamental の整数倍という制約」を満たしつつ、
// 「64 以上で最小の合法値」を選ぶ方針(= 切り上げ。要求を下回らない)。
// fundamental が 0(制約なし)の場合はそのままクランプするだけでよい。
inline uint32_t ChoosePeriodFrames(uint32_t want, uint32_t min, uint32_t max,
                                    uint32_t fundamental) {
    if (want < min) want = min;
    if (want > max) want = max;
    if (fundamental == 0) return want;

    // fundamental の倍数へ切り上げ(丸め込みで要求を下回らないようにする)。
    uint32_t periodFrames = ((want + fundamental - 1) / fundamental) * fundamental;
    if (periodFrames < min) periodFrames = min;
    if (periodFrames > max) periodFrames = max;
    return periodFrames;
}

}  // namespace wasapi
