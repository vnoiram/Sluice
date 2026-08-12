#pragma once
// wasapi_device.h : WASAPI 共有モードキャプチャ/レンダー(IAudioDevice 実装)
//
// 実装ガイド §5.2:
//   - 共有モードをデフォルトにする(排他はデバイスを奪うのでルーティング
//     アプリと相性が悪い)。IAudioClient3::InitializeSharedAudioStream で
//     対応デバイスなら小さい周期を要求する(非対応時は通常の
//     IAudioClient::Initialize にフォールバック)。
//   - 実装ガイド §5.2.5「排他モード」: DeviceStreamConfig::exclusiveMode を
//     true にすると AUDCLNT_SHAREMODE_EXCLUSIVE で開く(既定は false)。
//     共有モードでは 64 サンプルが出せないデバイスを救うための最終手段。
//     どちらのモードを試すかは呼び出し側の判断で、Probe() 自体は常に
//     共有モード相当の情報を返す(排他モードの事前問い合わせは行わない)。
//   - イベント駆動(AUDCLNT_STREAMFLAGS_EVENTCALLBACK)。gap 5: 実装ガイドが
//     64 サンプル運用で推奨する RTWQ(device/rtwq_worker.h、
//     RtwqLockSharedWorkQueue(L"Pro Audio", ...))を優先し、対応 OS でない
//     場合やセットアップに失敗した場合だけ、従来の専用 std::thread +
//     AvSetMmThreadCharacteristics(L"Pro Audio", ...) にフォールバックする
//     (OBS Studio の win-wasapi.cpp と同じ方針。RTWQ は Win 8.1+ 限定かつ
//     実運用で稀に問題が報告されているため、必ずフォールバック経路を残す)。
//   - 共有モードのミックスフォーマット(サンプルレート・チャンネル数)は
//     決め打ちしない。GetMixFormat の結果をそのまま使い、
//     DeviceStreamConfig の要求値との差はエンジン境界の ASRC が吸収する
//     (ASIO と同じ仕組みで統一的に同期できる)。
//   - デバイスのホットプラグは IMMNotificationClient で検知し、
//     resetRequested を立てる(呼び出し側が Close→Open で作り直す。
//     ASIO の kAsioResetRequest と同じ扱い)。
//   - WASAPI loopback capture(AUDCLNT_STREAMFLAGS_LOOPBACK): レンダー
//     エンドポイントに送られた音声をそのエンドポイント自身から録音できる
//     OS 標準機能(対象オーディオドライバ側の対応は不要)。コンストラクタの
//     loopback 引数で有効化する(docs/audio-router-implementation-gap.md
//     gap 9: 録音側エンドポイントを別途持たない仮想デバイス — 例
//     VirtualDrivers/Virtual-Audio-Driver — の実測用途)。仕様上シェアード
//     モード専用(排他モードとは併用不可、Open() で拒否する)。
//
// 罠: WASAPI のバッファは常にインターリーブ(チャンネルごとに独立した
// バッファではない)。IAudioDevice 契約はプレーナなので、キャプチャ時は
// デインターリーブ、レンダー時はインターリーブへの変換をここで行う。
//
// 罠: WASAPI 共有モードのイベント 1 回あたりの利用可能フレーム数は
// ASIO と違い固定ではない(GetCurrentPadding 依存)。そのため
// SetBlockCallback のコールバックには毎回そのブロックの実フレーム数を
// 渡す(IAudioDevice::SetBlockCallback のコメント参照)。

#include <windows.h>
#include <objbase.h>   // COM 基礎(asio_host.h と同じ理由。WIN32_LEAN_AND_MEAN 対策)
#include <mmreg.h>     // WAVEFORMATEX/WAVEFORMATEXTENSIBLE, WAVE_FORMAT_* 定数
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "device/iaudio_device.h"
#include "device/rtwq_worker.h"  // gap 5: RTWQ 経路(利用不可時は std::thread にフォールバック)
#include "rt/spsc_ring.h"

namespace wasapi {

struct EndpointInfo {
    std::wstring id;     // IMMDevice::GetId() の文字列(WasapiDevice のコンストラクタへ渡す)
    std::wstring name;   // フレンドリ名(表示用)
};

// isCapture=true で eCapture、false で eRender 側のエンドポイントを列挙する。
std::vector<EndpointInfo> EnumerateEndpoints(bool isCapture);

// IAudioDevice 実装。1 インスタンス = 1 方向(キャプチャ or レンダー)の
// WASAPI 共有モードストリーム。
class WasapiDevice : public IAudioDevice {
public:
    // endpointId が空文字列なら既定デバイス(eConsole ロール)を使う。
    //
    // loopback=true(WASAPI loopback capture、Windows のオーディオエンジンが
    // 標準で提供する機能で、対象ドライバ側の対応は不要): endpointId は
    // レンダー(eRender)側エンドポイントを指し、そこへ再生された音声を
    // キャプチャする。isCapture の値に関わらず(コンストラクタ内で
    // isCapture_ = true に強制する)IAudioCaptureClient 経由で読む
    // 通常のキャプチャデバイスとして振る舞う(CaptureRing() から読める)。
    // WASAPI の仕様上、排他モード(DeviceStreamConfig::exclusiveMode)とは
    // 併用できない(Open() で拒否する)。
    //
    // 用途: VAC/VB-CABLE のように録音側エンドポイントを別途持たない仮想
    // オーディオドライバ(例: レンダー専用の仮想スピーカーのみを持つドライバ)
    // の実測に使う。tools/latencybench の --loopback 参照。
    WasapiDevice(std::wstring endpointId, bool isCapture, bool loopback = false);
    ~WasapiDevice() override { Close(); }
    WasapiDevice(const WasapiDevice&) = delete;

    // 実装ガイド §5.2.1 の手順(SetClientProperties→GetMixFormat→
    // GetSharedModeEnginePeriod→64 の合法性判定)だけを行い、フルの
    // Initialize/Start はしない。IAudioClient3 非対応デバイスでは
    // supports64=false・recommendedLane=Compat を返す。
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
    UINT32 PeriodFrames() const { return periodFrames_; }

    // ホットプラグ通知クライアント(.cpp 内で実体を定義)から呼ばれる。
    void RequestReset() { resetRequested_.store(true); }

private:
    // gap 5: RTWQ(利用不可時は std::thread にフォールバック、
    // rtwq_worker.h の設計コメント参照)経由で1回分の処理をトリガする。
    class RtwqCallback : public rtwq::AsyncCallback {
    public:
        explicit RtwqCallback(WasapiDevice* owner) : owner_(owner) {}
        HRESULT STDMETHODCALLTYPE Invoke(IRtwqAsyncResult* result) override;

    private:
        WasapiDevice* owner_;
    };

    void ThreadMain();       // フォールバック経路: 専用スレッド上のイベント駆動ループ
    void OnAudioSignal();    // 1 イベントぶんの処理本体(RTWQ/std::thread 両経路が呼ぶ共通部分)
    void ProcessOneCapture();  // GetBuffer 1回分: デインターリーブして CaptureRing へ
    void ProcessOneRender(UINT32 availableFrames);  // RenderRing から読みインターリーブして GetBuffer へ

    // 実装ガイド §5.2.1 の周期問い合わせ手順を Probe()/Open() で共有する
    // ためのヘルパ。client は Activate 済み(Initialize 前)の IAudioClient。
    // rawMode が true なら AUDCLNT_STREAMOPTIONS_RAW を要求する
    // (実装ガイド §5.2.4)。成功したら true を返し、各 out 引数に
    // GetSharedModeEnginePeriod の結果を書く。IAudioClient3 非対応なら false。
    static bool QuerySharedModePeriod(IAudioClient* client, WAVEFORMATEX* mixFormat,
                                       bool rawMode, UINT32* defaultPeriod,
                                       UINT32* fundamentalPeriod, UINT32* minPeriod,
                                       UINT32* maxPeriod);

    // 排他モード(実装ガイド §5.2.5)用の候補フォーマットを組み立てる。
    // GetMixFormat が使えない(共有エンジン専用の概念のため)ので、
    // config から直接組み立てて IsFormatSupported(EXCLUSIVE, ...) で
    // 確認する側で使う。floatFormat=false なら 16bit PCM を組み立てる
    // (float32 が拒否された場合のフォールバック用)。
    static WAVEFORMATEXTENSIBLE BuildCandidateFormat(const DeviceStreamConfig& config,
                                                      bool floatFormat);

    std::wstring endpointId_;
    bool isCapture_;
    bool loopback_ = false;

    IMMDeviceEnumerator* enumerator_ = nullptr;
    IMMDevice* device_ = nullptr;
    IAudioClient* audioClient_ = nullptr;
    IAudioCaptureClient* captureClient_ = nullptr;
    IAudioRenderClient* renderClient_ = nullptr;

    int channels_ = 0;
    double sampleRate_ = 0.0;
    UINT32 periodFrames_ = 0;     // GetBufferSize() が返すエンジンバッファサイズ(上限の目安)
    bool formatIsFloat_ = true;   // false なら PCM 整数(16/24/32bit)
    int bitsPerSample_ = 32;

    std::vector<std::unique_ptr<SpscRing<float>>> rings_;  // channels_ 個、プレーナ
    // チャンネルごとに periodFrames_ 個ずつ並べたプレーナ・スクラッチ
    // (channelScratch_[c*periodFrames_ + f])。WASAPI バッファ(インター
    // リーブ)⇔ リング(プレーナ)変換の中継用。起動前確保、RT 中は伸びない。
    std::vector<float> channelScratch_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    HANDLE audioEvent_ = nullptr;
    HANDLE stopEvent_ = nullptr;

    // gap 5: RTWQ 経路(rtwq_worker.h 参照)。rtwqActive_ が true の間は
    // thread_/ThreadMain ではなくこちらが audioEvent_ を監視する。
    // セットアップ失敗時(古い Windows、RTWorkQ.dll 無し等)は
    // rtwqActive_ = false のまま既存の thread_ 経路にフォールバックする。
    RtwqCallback rtwqCallback_{this};
    IRtwqAsyncResult* rtwqAsyncResult_ = nullptr;
    DWORD rtwqQueueId_ = 0;
    HANDLE rtwqIdleEvent_ = nullptr;  // Stop() が RTWQ 側の停止完了を待つための同期イベント
    bool rtwqActive_ = false;

    std::function<void(int frames)> blockCallback_;
    std::atomic<uint64_t> cbCount_{0};
    std::atomic<uint64_t> underrunCount_{0};
    std::atomic<uint64_t> overrunCount_{0};
    std::atomic<bool> resetRequested_{false};
    double latencySeconds_ = 0.0;
    Lane lane_ = Lane::Compat;  // Open() 完了時に確定(periodFrames_ から判定)

    // IMMNotificationClient 実装(実体は .cpp 内の WasapiDevice::NotificationClient)。
    // 生ポインタで保持し、Close() で Unregister + Release する。
    IMMNotificationClient* notifyClient_ = nullptr;
    class NotificationClient;
};

}  // namespace wasapi
