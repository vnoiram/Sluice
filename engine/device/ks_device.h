#pragma once
// ks_device.h : DirectKS (Kernel Streaming) キャプチャ/レンダー(IAudioDevice 実装)
//               実装ガイド §6「Phase 1.5: KS (DirectKS) バックエンド」
//
// 初学者向け: 最初に詰まる罠
//   - KS ピンは基本的に排他的。WASAPI 共有モード(wasapi_device.h)と違い、
//     既に他のクライアントが掴んでいるピンは開けない。同一デバイスを
//     WasapiDevice と KsDevice の両方から同時に開こうとすると失敗しうる。
//   - `SetupDiGetClassDevs(KSCATEGORY_AUDIO)` はオーディオ「フィルタ」を
//     列挙するだけで、入出力方向やフォーマットはピンを辿らないと分からない
//     (フィルタ 1 個が capture ピンと render ピンの両方を持つことがある)。
//   - `KSPROPERTY_PIN_PROPOSEDATAFORMAT` はピンの `KSPROPERTY_PIN_DATARANGES`
//     が対応を謳っているフォーマットでも拒否されることがある。提案が通る
//     フォーマットを見つけるまで複数候補を試す前提で書く。
//   - gap 5/7: WaveRT(`KSPROPERTY_RTAUDIO_BUFFER_WITH_NOTIFICATION` による
//     循環バッファの直接マップ)に対応した。ピン生成時に
//     `KSINTERFACE_STANDARD_LOOPED_STREAMING` を先に試し、成功かつ
//     WaveRT のバッファ/通知イベント取得(下記 TryEnableWaveRt)にも
//     成功した場合だけ使う。どちらかが失敗したら、そのピンは破棄して
//     従来どおり `KSINTERFACE_STANDARD_STREAMING` +
//     `ReadFile`/`WriteFile` + `KSSTREAM_HEADER` にフォールバックする
//     (素のストリーミング I/O は KS ピンを持つ実質すべてのオーディオ
//     デバイスで動く共通の最小分母、実装ガイド §6.2)。
//     参考にした実装: PortAudio の WDM-KS バックエンド
//     (src/hostapi/wdmks/pa_win_wdmks.c)。本実装は同ファイルの
//     "WaveRT Event" サブモード相当(NotificationCount=2 の仕様どおり、
//     通知のたびにバッファの半分ぶんちょうど処理する簡略版)で、
//     PortAudio がさらに行っているハードウェア位置レジスタに基づく
//     厳密な追跡("WaveRT Polled" 相当)は実装していない
//     (実機での追加検証が必要な将来課題として残す)。
//   - `KSPROPERTY_CONNECTION_STATE` は KSSTATE_STOP → ACQUIRE → PAUSE → RUN
//     の順で 1 段階ずつ遷移させる必要がある(いきなり RUN にはできない)。
//     停止時も RUN → PAUSE → ACQUIRE → STOP の順で戻す。
//
// レーン分類: 実装ガイド §6.2「常に RT Lane 候補」。OS のミキシングエンジンを
// 経由しない分、WASAPI 共有モードより最小遅延を狙えるが、デバイスを排他的に
// 掴む点は ASIO と同種のトレードオフ。

#include <windows.h>
#include <objbase.h>   // COM 基礎(asio_host.h/wasapi_device.h と同じ理由)
#include <mmreg.h>     // WAVEFORMATEX
#include <setupapi.h>  // SetupDiGetClassDevs 等(setupapi.lib)

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "device/iaudio_device.h"
#include "rt/spsc_ring.h"

namespace ks {

struct KsDeviceInfo {
    std::wstring symbolicLink;  // CreateFile に渡すデバイスインターフェースパス
    std::wstring friendlyName;  // 表示用
};

// KSCATEGORY_AUDIO のデバイスインターフェースを列挙する。
// (SetupDiGetClassDevs はフィルタを返すだけで、入出力方向はまだ分からない
// 点に注意。方向の判定は OpenPin 時にピンを辿って行う)
std::vector<KsDeviceInfo> EnumerateKsAudioDevices();

// IAudioDevice 実装。1 インスタンス = 1 方向(キャプチャ or レンダー)の
// KS ピン 1 本(モノラルまたは指定チャンネル数の単一ピン)。
class KsDevice : public IAudioDevice {
public:
    KsDevice(KsDeviceInfo info, bool isCapture);
    ~KsDevice() override { Close(); }
    KsDevice(const KsDevice&) = delete;

    // 実装ガイド §6.2「常に RT Lane 候補」。フィルタを開いて要求方向の
    // ピンが存在するかだけを軽く確認する(フォーマット交渉・KsCreatePin は
    // 行わない。ピンは排他的なため、無駄にオープンして他クライアントを
    // ブロックしないため)。既に Open 済みならそれを再利用する。
    DeviceCaps Probe(double sampleRate) override;

    bool Open(const DeviceStreamConfig& config, std::wstring* errorOut) override;
    void Start() override;
    void Stop() override;
    void Close() override;

    SpscRing<float>* CaptureRing(int ch) override;
    SpscRing<float>* RenderRing(int ch) override;
    DeviceStatus Status() const override;
    void SetBlockCallback(std::function<void(int frames)> fn) override {
        blockCallback_ = std::move(fn);
    }

    int Channels() const { return channels_; }
    double SampleRate() const { return sampleRate_; }

private:
    // --- ピン探索・生成(実装ガイド §6.1 手順1〜4) ----------------------
    bool OpenFilter(std::wstring* errorOut);
    // desiredPeriodFrames: gap 7 の WaveRT 経路が要求バッファサイズ
    // (周期の2倍、TryEnableWaveRt 参照)を組み立てるのに使う希望値。
    // WaveRT が無効な場合は Open() 側が periodFrames_ に直接使う。
    bool FindAndCreatePin(const DeviceStreamConfig& config, UINT32 desiredPeriodFrames,
                          std::wstring* errorOut);
    bool SetPinState(ULONG state);  // KSSTATE_STOP/ACQUIRE/PAUSE/RUN へ1段階ずつ遷移

    // gap 7: pinHandle_ が KSINTERFACE_STANDARD_LOOPED_STREAMING で生成
    // できた直後にだけ呼ぶ。KSPROPERTY_RTAUDIO_BUFFER_WITH_NOTIFICATION で
    // 循環バッファを取得し、KSPROPERTY_RTAUDIO_REGISTER_NOTIFICATION_EVENT
    // で通知イベントを登録する。要求バッファは periodFrames_ の 2 倍
    // (NotificationCount=2: 半周・1周で通知が来る)。失敗したら
    // waveRt*_ 系メンバは初期状態のまま false/nullptr で返る
    // (呼び出し側がこのピンを破棄してフォールバックする)。
    bool TryEnableWaveRt(UINT32 desiredPeriodFrames, std::wstring* errorOut);

    // --- RT(専用スレッド)側 ---------------------------------------------
    void ThreadMain();
    void ProcessOneCapture();
    void ProcessOneRender();
    // gap 7: WaveRT 経路(waveRtActive_ == true のときだけ使う)。
    // 通知イベントが 1 回シグナルされるたびに、マップ済み循環バッファの
    // 半分ぶん(periodFrames_ フレーム)を処理する。
    void ProcessOneWaveRtCapture();
    void ProcessOneWaveRtRender();

    KsDeviceInfo info_;
    bool isCapture_;

    HANDLE filterHandle_ = INVALID_HANDLE_VALUE;
    HANDLE pinHandle_ = INVALID_HANDLE_VALUE;
    ULONG pinId_ = 0;
    ULONG currentPinState_ = 0;  // KSSTATE_STOP を初期値とする(ks_device.cpp で定義)

    int channels_ = 0;
    double sampleRate_ = 0.0;
    int bitsPerSample_ = 32;      // ASIOSTFloat32LSB 相当、WAVE_FORMAT_IEEE_FLOAT を優先要求
    bool formatIsFloat_ = true;
    UINT32 periodFrames_ = 0;     // 1 回の ReadFile/WriteFile で扱うフレーム数

    std::vector<std::unique_ptr<SpscRing<float>>> rings_;  // channels_ 個、プレーナ
    std::vector<float> channelScratch_;    // インターリーブ⇔プレーナ変換の中継用
    std::vector<uint8_t> streamBuffer_;    // KSSTREAM_HEADER + PCM/float 生データ

    std::thread thread_;
    std::atomic<bool> running_{false};
    HANDLE stopEvent_ = nullptr;

    // gap 7: WaveRT 経路。waveRtActive_ が true の間、ThreadMain は
    // ReadFile/WriteFile ではなく waveRtNotifyEvent_ を待ってマップ済み
    // バッファを直接読み書きする(rtwq_worker.h とは無関係。KS は現状
    // 素の std::thread のまま、gap 5 の RTWQ 対応は wasapi_device.cpp の
    // みが対象 — ks_device.h 冒頭コメント参照)。
    bool waveRtActive_ = false;
    void* waveRtBuffer_ = nullptr;       // KsCreatePin したピンが自動的に所有(明示 unmap 不要)
    ULONG waveRtBufferSize_ = 0;         // ActualBufferSize(バイト数、周期の2倍ぶん)
    bool waveRtCallMemoryBarrier_ = false;
    HANDLE waveRtNotifyEvent_ = nullptr;
    int waveRtNextHalf_ = 0;             // 次に処理すべき半分(0 or 1)

    std::function<void(int frames)> blockCallback_;
    std::atomic<uint64_t> cbCount_{0};
    std::atomic<uint64_t> underrunCount_{0};
    std::atomic<uint64_t> overrunCount_{0};
    std::atomic<bool> resetRequested_{false};
    double latencySeconds_ = 0.0;
};

}  // namespace ks
