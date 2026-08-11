#pragma once
// vasio_driver.h : 仮想 ASIO ドライバ本体(実装ガイド §8.1)
//
// 初学者向け: 最初に詰まる罠
//   - これは「ドライバ」と名乗るがカーネルコードではない。実体はレジストリ
//     (HKCR\CLSID\{...} と HKLM\SOFTWARE\ASIO\<name>)に登録された、ただの
//     ユーザーモード COM DLL。DAW は CoCreateInstance でこれを生成し、
//     IASIO インターフェース越しに呼び出す(engine/device/asio_host.h が
//     「ホスト側」から他社ドライバを呼ぶのと対称の、逆方向の実装)。
//   - IASIO は IUnknown 以外 __thiscall の非標準 COM。ASIO SDK 付属の
//     combase.h/.cpp(CUnknown/CFactoryTemplate)がこの COM ボイラープレートを
//     提供してくれるので自作しない(driver/asiosample/asiosmpl.cpp が実例)。
//   - createBuffers() で渡される bufferSize は「エンジンが今動いている
//     ブロックサイズ」と一致するとは限らない。DAW 側が要求したサイズを
//     そのまま受け入れ、エンジン側のブロックサイズとの差はエンジン境界の
//     ASRC が吸収する設計(実装ガイド §2.3 と同じ非対称)。本ドライバ自体は
//     リサンプルを一切行わない。
//   - bufferSwitch は「エンジンのマスタークロックに従属」する
//     (実装ガイド §8.1 手順4)。DAW 用の独自タイマは持たない。エンジンが
//     未接続/応答なしの間は、DAW を待たせないためタイムアウト付きで無音を
//     返し続ける(手順5「切断耐性」)。
//   - kAsioResetRequest は「今度は自分がドライバ側の作法を実装する立場」
//     (実装ガイド §8.1 手順6)。engine/device/asio_host.cpp が他社ドライバ
//     からこれを受け取る側だったのに対し、ここでは DAW へ送出する側になる。
//
// スコープ: 本実装は ASIOSTFloat32LSB 固定・timeInfo モード非対応の最小構成。
// 既定 8in/8out(kMaxChannels、shared_protocol.h)。マルチインスタンス(複数
// CLSID を跨いだ同時起動)は将来課題。

#include "asiosys.h"
#include "combase.h"
#include "iasiodrv.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <thread>

#include "shared_protocol.h"

// {A1B2C3D4-1234-4E56-8F9A-0123456789AB} : Sluice vasio 固有の CLSID。
// 実装ガイド §12「プロジェクト名は... 衝突確認してから確定する」に対応する
// 正式な値ではなく、開発用の仮 CLSID。製品化前に必ず新規採番すること。
// driver/asiosample/asiosmpl.cpp の IID_ASIO_DRIVER と同じ流儀(素の CLSID 定数、
// DEFINE_GUID マクロは使わない — INITGUID の定義漏れによるリンクエラーを避ける)。
extern CLSID CLSID_SluiceVasio;

class SluiceVasioDriver : public IASIO, public CUnknown {
public:
    SluiceVasioDriver(LPUNKNOWN pUnk, HRESULT* phr);
    ~SluiceVasioDriver();

    DECLARE_IUNKNOWN

    static CUnknown* CreateInstance(LPUNKNOWN pUnk, HRESULT* phr);
    virtual HRESULT STDMETHODCALLTYPE NonDelegatingQueryInterface(REFIID riid, void** ppvObject);

    // --- IASIO --------------------------------------------------------
    ASIOBool init(void* sysHandle) override;
    void getDriverName(char* name) override;
    long getDriverVersion() override;
    void getErrorMessage(char* string) override;
    ASIOError start() override;
    ASIOError stop() override;
    ASIOError getChannels(long* numInputChannels, long* numOutputChannels) override;
    ASIOError getLatencies(long* inputLatency, long* outputLatency) override;
    ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize,
                             long* granularity) override;
    ASIOError canSampleRate(ASIOSampleRate sampleRate) override;
    ASIOError getSampleRate(ASIOSampleRate* sampleRate) override;
    ASIOError setSampleRate(ASIOSampleRate sampleRate) override;
    ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) override;
    ASIOError setClockSource(long reference) override;
    ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) override;
    ASIOError getChannelInfo(ASIOChannelInfo* info) override;
    ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize,
                             ASIOCallbacks* callbacks) override;
    ASIOError disposeBuffers() override;
    ASIOError controlPanel() override;
    ASIOError future(long selector, void* opt) override;
    ASIOError outputReady() override;

private:
    // --- 共有メモリ接続(実装ガイド §8.1 手順3・手順5) -----------------
    bool ConnectSharedMemory();     // init() から呼ぶ。失敗しても致命的にしない
    void DisconnectSharedMemory();  // ~SluiceVasioDriver()/エラー時

    // --- RT ワーカースレッド(実装ガイド §8.1 手順4) -------------------
    void WorkerThreadMain();
    void PumpOneBuffer();  // ワーカースレッドから呼ばれる 1 ブロック分の処理

    HANDLE mapping_ = nullptr;
    HANDLE readyEvent_ = nullptr;
    void* mappedView_ = nullptr;
    vasio::SharedMemoryLayout layout_{};
    vasio::SharedControlBlock* control_ = nullptr;
    vasio::ChannelRingHeader* ringHeaders_ = nullptr;  // 2*kMaxChannels 本
    float* ringData_ = nullptr;                        // 2*kMaxChannels 本ぶんの領域

    std::thread worker_;
    std::atomic<bool> workerRunning_{false};
    HANDLE stopEvent_ = nullptr;

    // --- ASIO 側状態 ----------------------------------------------------
    ASIOCallbacks* callbacks_ = nullptr;
    long blockFrames_ = 512;
    long activeInputs_ = 0;   // DAW にとっての ASIO 入力 = FromEngine
    long activeOutputs_ = 0;  // DAW にとっての ASIO 出力 = ToEngine
    long inMap_[vasio::kMaxChannels]{};
    long outMap_[vasio::kMaxChannels]{};
    float* inputBuffers_[vasio::kMaxChannels]{};   // ダブルバッファ、blockFrames_*2 個ずつ
    float* outputBuffers_[vasio::kMaxChannels]{};
    long toggle_ = 0;
    double sampleRate_ = 48000.0;
    bool started_ = false;
    bool active_ = false;
    char errorMessage_[128] = "";

    // 直前に DAW へ kAsioResetRequest を送出済みかどうか(多重送出防止)。
    bool resetNotified_ = false;
};
