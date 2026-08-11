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
// §2.4)。1 チャンネル = 1 SpscRing<float>。
//
// マスタークロックとの同期: SetBlockCallback で登録したコールバックは、
// このデバイスの RT コールバックが 1 ブロック処理するたびに、その
// ブロックのフレーム数(frames)を引数に呼ばれる。
//   - 入力デバイス: CaptureRing へ書き込んだ**あとに**呼ばれる
//     (「新しい入力データが来た」通知。現状の実装では未使用でも良い)。
//   - 出力デバイス: RenderRing から読み出す**前に**呼ばれる
//     (エンジンはここで RenderRing に新しいブロックを書き込む。
//     実装ガイド §5.4.1 の「マスターコールバック」はこれに相当する)。
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

    // 実装ガイド §5.2.3「積極的低遅延モードはオプトイン」: false(既定)なら
    // IAudioClient3 の小バッファ要求パスを使わず、デバイス既定周期で開く。
    // true にした場合のみ、上の preferredBufferFrames を使った低遅延要求を
    // 試みる。小バッファ要求は同じエンドポイントを使う他アプリを巻き込む
    // ため、既定でオフにしておく(WasapiDevice のみが参照する)。
    bool   aggressiveLowLatency = false;

    // 実装ガイド §5.2.4「RAW モード」: true にすると APO(デバイス固有 EQ・
    // 音量補正・空間オーディオ)をバイパスする。既定はオフ(WasapiDevice の
    // みが参照する)。
    bool   rawMode = false;
};

// 実装ガイド §2.3「レーン設計」の中核概念。全経路を 64 サンプルに揃えるのは
// 不可能かつ不要なので、経路を RT Lane(低遅延)/Compat Lane(汎用)に
// 明示的に分離する。レーンの違いは「デバイスとエンジンの境界にあるリングの
// 容量と ASRC の有無」だけであり、エンジン本体はレーンを意識しない。
enum class Lane {
    RT,       // ASIO / DirectKS / WASAPI(64 達成) / VAC(1ms 設定) 等
    Compat,   // VB-CABLE / Voicemeeter VAIO / WASAPI 既定周期 / プロセスループバック等
};

// デバイスの能力プローブ結果(実装ガイド §5.1・§5.2.1)。Open() する前に
// 「このデバイスは 64 サンプルで動かせるか」「どのレーンが妥当か」を
// 判定するための情報。フル Open/Start はせず、可能な限り軽量に問い合わせる。
struct DeviceCaps {
    uint32_t minPeriodFrames = 0;      // デバイスが申告する到達可能な最小周期
    uint32_t fundamentalFrames = 0;    // 周期の粒度(WASAPI のみ意味を持つ。0 = 不明/制約なし)
    uint32_t defaultPeriodFrames = 0;  // デバイス既定の周期
    bool     supports64 = false;       // 64 サンプルで駆動可能か
    double   measuredLatencyMs = 0.0;  // 実測値(0 = 未測定。tools/latencybench が埋める想定)
    Lane     recommendedLane = Lane::Compat;
};

struct DeviceStatus {
    uint64_t callbackCount = 0;
    uint64_t underrunCount = 0;   // レンダー側: RenderRing が空で無音を出した回数
    uint64_t overrunCount = 0;    // キャプチャ側: CaptureRing が満杯で書き切れなかった回数
    long     bufferSizeFrames = 0;
    double   effectiveLatencySeconds = 0.0;
    bool     resetRequested = false;  // kAsioResetRequest 相当。true なら Close→Open で作り直す

    // 実際に Open() した結果として確定した、このデバイスが動作しているレーン。
    // DeviceCaps::recommendedLane(測定に基づく推奨値)とは意味が異なる点に注意。
    Lane     lane = Lane::Compat;
};

class IAudioDevice {
public:
    virtual ~IAudioDevice() = default;

    // Open() の前に呼べる軽量な能力問い合わせ(実装ガイド §5.1・§5.2.1)。
    // フルの Open/Start は行わず、可能なら破棄可能な一時ハンドルだけで
    // 判定する。sampleRate は問い合わせたい対象レート。
    virtual DeviceCaps Probe(double sampleRate) = 0;

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
