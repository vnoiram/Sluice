#pragma once
// process_loopback_device.h : プロセス別ループバックキャプチャ(IAudioDevice 実装)
//
// 実装ガイド §5.3:
//   Windows 10 2004+(Build 19041)の ActivateAudioInterfaceAsync +
//   AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK を使うと、特定プロセス
//   (と、includeChildren なら子プロセスツリー)の再生音「だけ」を
//   キャプチャできる。「ブラウザの音」「ゲームの音」を別ストリップに
//   する Voicemeeter 的ユースケースの入力側が、仮想デバイスなしで
//   実現できる。
//
//   - 出力フォーマットは呼び出し側(DeviceStreamConfig)が指定でき、
//     通常の WASAPI 共有モードと違い GetMixFormat に従う必要はない。
//     float32 で要求するので、wasapi_device.cpp のような PCM 変換は
//     不要(常に float32 のみを扱う)。
//   - 対象プロセスが音を出していない間は無音データが来るだけで、
//     エラーにはならない。
//   - プロセス終了は TargetProcessAlive() で検知できる。呼び出し側は
//     これをポーリングし、終了していたら Close→(新しい PID で)
//     再アタッチする(本クラスは自動再アタッチ自体は行わない。
//     resetRequested を Status() 経由で通知するだけ)。
//
// キャプチャ専用(RenderRing は常に nullptr)。イベント駆動+専用スレッド
// である点は wasapi_device.cpp と同じ(共通化は将来のリファクタ候補)。

#include <windows.h>
#include <objbase.h>
#include <mmreg.h>
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

class ProcessLoopbackDevice : public IAudioDevice {
public:
    // targetPid: 対象プロセスの PID。
    // includeChildren: true なら子プロセスツリーも含める(PROCESS_LOOPBACK_MODE_
    // INCLUDE_TARGET_PROCESS_TREE)、false ならそのプロセス単体のみ
    // (…_EXCLUDE_TARGET_PROCESS_TREE)。
    ProcessLoopbackDevice(DWORD targetPid, bool includeChildren);
    ~ProcessLoopbackDevice() override { Close(); }
    ProcessLoopbackDevice(const ProcessLoopbackDevice&) = delete;

    // ActivateAudioInterfaceAsync は IAudioClient3 の周期交渉に対応しない
    // ため、常に固定値(supports64=false, Lane::Compat)を返す
    // (実装ガイド §2.3: プロセスループバックは常に Compat Lane)。
    DeviceCaps Probe(double /*sampleRate*/) override {
        DeviceCaps caps;
        caps.recommendedLane = Lane::Compat;
        caps.supports64 = false;
        return caps;
    }

    // config.sampleRate/channels がそのまま要求フォーマット(float32)になる。
    bool Open(const DeviceStreamConfig& config, std::wstring* errorOut) override;
    void Start() override;
    void Stop() override;
    void Close() override;

    SpscRing<float>* CaptureRing(int ch) override;
    SpscRing<float>* RenderRing(int /*ch*/) override { return nullptr; }  // キャプチャ専用
    DeviceStatus Status() const override;
    void SetBlockCallback(std::function<void(int frames)> fn) override {
        blockCallback_ = std::move(fn);
    }

    // 対象プロセスがまだ生きているか。呼び出し側の監視ループが定期的に
    // 呼ぶことを想定(本クラス自身も RT スレッド内で毎ブロック確認し、
    // 終了を検知したら resetRequested を立てる)。
    bool TargetProcessAlive() const;

private:
    void ThreadMain();
    void ProcessOneCapture();

    DWORD targetPid_;
    bool includeChildren_;

    IAudioClient* audioClient_ = nullptr;
    IAudioCaptureClient* captureClient_ = nullptr;
    HANDLE targetProcessHandle_ = nullptr;

    int channels_ = 0;
    double sampleRate_ = 0.0;
    UINT32 periodFrames_ = 0;

    std::vector<std::unique_ptr<SpscRing<float>>> rings_;  // channels_ 個、プレーナ
    std::vector<float> channelScratch_;  // WASAPI(インターリーブ)⇔ プレーナ変換用

    std::thread thread_;
    std::atomic<bool> running_{false};
    HANDLE audioEvent_ = nullptr;
    HANDLE stopEvent_ = nullptr;

    std::function<void(int frames)> blockCallback_;
    std::atomic<uint64_t> cbCount_{0};
    std::atomic<uint64_t> overrunCount_{0};
    std::atomic<bool> resetRequested_{false};
    double latencySeconds_ = 0.0;

    class ActivationHandler;  // .cpp 内: IActivateAudioInterfaceCompletionHandler 実装
};

}  // namespace wasapi
