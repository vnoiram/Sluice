#pragma once
// wasapi_device.h : WASAPI 共有モードキャプチャ/レンダー(IAudioDevice 実装)
//
// 実装ガイド §5.2:
//   - 共有モードをデフォルトにする(排他はデバイスを奪うのでルーティング
//     アプリと相性が悪い)。IAudioClient3::InitializeSharedAudioStream で
//     対応デバイスなら小さい周期を要求する(非対応時は通常の
//     IAudioClient::Initialize にフォールバック)。
//   - イベント駆動(AUDCLNT_STREAMFLAGS_EVENTCALLBACK)+専用スレッド。
//     スレッドは AvSetMmThreadCharacteristics(L"Pro Audio", ...) で
//     MMCSS に登録し優先度を確保する。
//   - 共有モードのミックスフォーマット(サンプルレート・チャンネル数)は
//     決め打ちしない。GetMixFormat の結果をそのまま使い、
//     DeviceStreamConfig の要求値との差はエンジン境界の ASRC が吸収する
//     (ASIO と同じ仕組みで統一的に同期できる)。
//   - デバイスのホットプラグは IMMNotificationClient で検知し、
//     resetRequested を立てる(呼び出し側が Close→Open で作り直す。
//     ASIO の kAsioResetRequest と同じ扱い)。
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
    WasapiDevice(std::wstring endpointId, bool isCapture);
    ~WasapiDevice() override { Close(); }
    WasapiDevice(const WasapiDevice&) = delete;

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
    void ThreadMain();       // イベント駆動ループ本体(専用スレッド上で実行)
    void ProcessOneCapture();  // GetBuffer 1回分: デインターリーブして CaptureRing へ
    void ProcessOneRender(UINT32 availableFrames);  // RenderRing から読みインターリーブして GetBuffer へ

    std::wstring endpointId_;
    bool isCapture_;

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

    std::function<void(int frames)> blockCallback_;
    std::atomic<uint64_t> cbCount_{0};
    std::atomic<uint64_t> underrunCount_{0};
    std::atomic<uint64_t> overrunCount_{0};
    std::atomic<bool> resetRequested_{false};
    double latencySeconds_ = 0.0;

    // IMMNotificationClient 実装(実体は .cpp 内の WasapiDevice::NotificationClient)。
    // 生ポインタで保持し、Close() で Unregister + Release する。
    IMMNotificationClient* notifyClient_ = nullptr;
    class NotificationClient;
};

}  // namespace wasapi
