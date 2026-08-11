#pragma once
// master_clock.h : マスタークロック候補選定(実装ガイド §2.3)
//
// 「マスタークロックは RT Lane のデバイスからのみ選択可能」というレーン設計の
// 制約を実装する、小さく独立したフィルタ関数。EngineGraph/main.cpp への
// フル統合(README に記載の既知の未着手事項)はこのファイルのスコープ外 ——
// ここでは統合が行われる時点で使えるプリミティブだけを用意する。

#include <vector>

#include "device/iaudio_device.h"

namespace engine {

// candidates の中から Lane::RT で動作しているデバイスだけを返す(出現順を
// 維持する)。呼び出し側はこの中から実際にマスタークロックとして使う 1 台を
// 選ぶ(例: 先頭、あるいはユーザー指定)。RT Lane のデバイスが 1 つも無い
// 場合は空の vector を返す(実装ガイド §2.3「全デバイス停止時は高精度
// タイマで代替」は呼び出し側の責務)。
inline std::vector<IAudioDevice*> SelectMasterClockCandidates(
    const std::vector<IAudioDevice*>& candidates) {
    std::vector<IAudioDevice*> result;
    for (IAudioDevice* dev : candidates) {
        if (!dev) continue;
        if (dev->Status().lane == Lane::RT) result.push_back(dev);
    }
    return result;
}

}  // namespace engine
