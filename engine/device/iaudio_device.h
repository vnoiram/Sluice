#pragma once
// iaudio_device.h : デバイス抽象化(実装ガイド §5.1)
//
// ASIO / WASAPI / 将来の仮想デバイスを同一に扱うインターフェース。
//
// 責務の分担(重要):
//   - デバイス実装は「自分の RT スレッドで、自分のチャンネルごとの
//     SpscRing<float> に読み書きする」だけを担当する。
//   - サンプルレート変換(ASRC)・クロックドリフト補正は**デバイス側に
//     置かない**。エンジン側の境界(≒デバイス間の受け渡し部分)に置く
//     ことで、デバイス実装を単純に保つ(実装ガイド §5.1)。
//   - キャプチャリング(CaptureRing)は「デバイスが書き、エンジンが読む」。
//     レンダーリング(RenderRing)は「エンジンが書き、デバイスが読む」。
//
// 内部フォーマットは float32 / プレーナ(インターリーブなし、実装ガイド
// §2.3)。1 チャンネル = 1 SpscRing<float>。
//
// マスタークロックとの同期: SetBlockCallback で登録したコールバックは、
// このデバイスの RT コールバックが 1 ブロック処理するたびに、その
// ブロックのフレーム数(frames)を引数に呼ばれる。
//   - 入力デバイス: CaptureRing へ書き込んだ**あとに**呼ばれる
//     (「新しい入力データが来た」通知。現状の実装では未使用でも良い)。
//   - 出力デバイス: RenderRing から読み出す**前に**呼ばれる
//     (エンジンはここで RenderRing に新しいブロックを書き込む。
//     実装ガイド §5.4.2 の「マスターコールバック」はこれに相当する)。
//
// frames が固定値とは限らない点に注意: ASIO は毎回同じ bufferSizeFrames
// だが、WASAPI 共有モードはイベントごとに利用可能フレーム数が変動しうる
// (GetCurrentPadding 依存)。呼び出し側は毎回 frames を見て処理量を
// 決めること(固定値を決め打ちしない)。

#include <cstdint>
#include <functional>

#include "rt/spsc_ring.h"

struct DeviceStreamConfig {
    double sampleRate = 48000.0;
    int    channels = 2;
    // 0 の場合はデバイスの既定(preferred buffer size)に従う。
    // WASAPI 等、明示的な周期指定に対応するデバイスのみ参照する。
    long   preferredBufferFrames = 0;
};

struct DeviceStatus {
    uint64_t callbackCount = 0;
    uint64_t underrunCount = 0;   // レンダー側: RenderRing が空で無音を出した回数
    uint64_t overrunCount = 0;    // キャプチャ側: CaptureRing が満杯で書き切れなかった回数
    long     bufferSizeFrames = 0;
    double   effectiveLatencySeconds = 0.0;
    bool     resetRequested = false;  // kAsioResetRequest 相当。true なら Close→Open で作り直す
};

class IAudioDevice {
public:
    virtual ~IAudioDevice() = default;

    virtual bool Open(const DeviceStreamConfig& config, std::wstring* errorOut) = 0;
    virtual void Start() = 0;
    virtual void Stop() = 0;
    virtual void Close() = 0;

    // ch は 0..channels-1。方向が合わない場合(例: 出力専用デバイスに
    // CaptureRing を要求)は nullptr を返す。
    virtual SpscRing<float>* CaptureRing(int ch) = 0;
    virtual SpscRing<float>* RenderRing(int ch) = 0;

    virtual DeviceStatus Status() const = 0;

    // ブロック境界通知(上記コメント参照)。RT スレッドから呼ばれるため、
    // 登録するコールバック自体も RT セーフ(アロケーションなし等)であること。
    virtual void SetBlockCallback(std::function<void(int frames)> fn) = 0;
};
