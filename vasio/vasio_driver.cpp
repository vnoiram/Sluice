// vasio_driver.cpp : 仮想 ASIO ドライバ本体の実装(実装ガイド §8.1)
//
// driver/asiosample/asiosmpl.cpp(Steinberg 提供のサンプルドライバ)を土台にした
// 構成。異なるのは「テスト波形を生成する」代わりに「共有メモリ越しに engine
// プロセスとデータを交換する」点のみ。COM ボイラープレート(CFactoryTemplate,
// CClassFactory, DllGetClassObject/DllCanUnloadNow/DllEntryPoint)は ASIO SDK の
// common/dllentry.cpp + common/combase.cpp をそのままリンクして使う
// (engine/CMakeLists.txt が SDK の .cpp を一切使わないホスト側とは対照的に、
// ドライバ側はこの SDK 提供の COM ヘルパー抜きでは書けない)。

#include "vasio_driver.h"

#include <cstdio>
#include <cstring>

// ===========================================================================
// CLSID 登録テーブル / ファクトリ(driver/asiosample/asiosmpl.cpp と同じ形)
// ===========================================================================

// {A1B2C3D4-1234-4E56-8F9A-0123456789AB}(開発用の仮 CLSID。vasio_driver.h 参照)
CLSID CLSID_SluiceVasio = {
    0xa1b2c3d4, 0x1234, 0x4e56, {0x8f, 0x9a, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab}};

CFactoryTemplate g_Templates[1] = {
    {L"Sluice Virtual ASIO", &CLSID_SluiceVasio, SluiceVasioDriver::CreateInstance}};
int g_cTemplates = sizeof(g_Templates) / sizeof(g_Templates[0]);

CUnknown* SluiceVasioDriver::CreateInstance(LPUNKNOWN pUnk, HRESULT* phr) {
    return static_cast<CUnknown*>(new SluiceVasioDriver(pUnk, phr));
}

HRESULT STDMETHODCALLTYPE SluiceVasioDriver::NonDelegatingQueryInterface(REFIID riid, void** ppv) {
    if (riid == CLSID_SluiceVasio) {
        return GetInterface(static_cast<IASIO*>(this), ppv);
    }
    return CUnknown::NonDelegatingQueryInterface(riid, ppv);
}

// ===========================================================================
// COM 登録/解除(実装ガイド §8.1 手順2)
//   HKCR\CLSID\{...}\InProcServer32 と HKLM\SOFTWARE\ASIO\<name> の両方を
//   ASIO SDK 付属の RegisterAsioDriver/UnregisterAsioDriver(common/register.cpp)
//   に任せる。DLL 名・表示名はここでしか出てこないので、ビルド成果物の実際の
//   ファイル名(vasio.dll)と一致させること。
// ===========================================================================

extern "C" LONG RegisterAsioDriver(CLSID, char*, char*, char*, char*);
extern "C" LONG UnregisterAsioDriver(CLSID, char*, char*);

extern "C" HRESULT __stdcall DllRegisterServer() {
    LONG rc = RegisterAsioDriver(CLSID_SluiceVasio, const_cast<char*>("vasio.dll"),
                                  const_cast<char*>("Sluice Virtual ASIO"),
                                  const_cast<char*>("Sluice Virtual ASIO"),
                                  const_cast<char*>("Apartment"));
    if (rc) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "Register Server failed! (%ld)", rc);
        MessageBoxA(nullptr, msg, "Sluice Virtual ASIO", MB_OK);
        return E_FAIL;
    }
    return S_OK;
}

extern "C" HRESULT __stdcall DllUnregisterServer() {
    LONG rc = UnregisterAsioDriver(CLSID_SluiceVasio, const_cast<char*>("vasio.dll"),
                                    const_cast<char*>("Sluice Virtual ASIO"));
    if (rc) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "Unregister Server failed! (%ld)", rc);
        MessageBoxA(nullptr, msg, "Sluice Virtual ASIO", MB_OK);
        return E_FAIL;
    }
    return S_OK;
}

// common/dllentry.cpp は DLL_PROCESS_ATTACH/DETACH のフックとして
// DllEntryPoint(DllMain ではない)を定義している。MSVC の DLL エントリポイントは
// 名前が正確に DllMain のものを自動的に採用するため、ここで単純に転送する。
extern "C" BOOL WINAPI DllEntryPoint(HINSTANCE, ULONG, LPVOID);
extern "C" BOOL WINAPI DllMain(HINSTANCE hInst, ULONG reason, LPVOID reserved) {
    return DllEntryPoint(hInst, reason, reserved);
}

// ===========================================================================
// 構築/破棄
// ===========================================================================

SluiceVasioDriver::SluiceVasioDriver(LPUNKNOWN pUnk, HRESULT* phr)
    : CUnknown(const_cast<TCHAR*>(TEXT("SluiceVasio")), pUnk, phr) {
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

SluiceVasioDriver::~SluiceVasioDriver() {
    stop();
    disposeBuffers();
    DisconnectSharedMemory();
    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
}

// ===========================================================================
// 共有メモリ接続(実装ガイド §8.1 手順3・手順5)
// ===========================================================================

bool SluiceVasioDriver::ConnectSharedMemory() {
    // ring 容量は「エンジン側ブロックサイズの数十倍」を確保しておき、DAW 側の
    // 要求バッファサイズ(createBuffers 時に変わりうる)より十分大きく取る。
    // 実装ガイド §4.2 の RT Lane 容量係数(ブロック×32〜64)に合わせる。
    constexpr uint32_t kRingCapacityFrames = 8192;
    layout_ = vasio::ComputeLayout(kRingCapacityFrames);

    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                   static_cast<DWORD>(layout_.totalBytes), vasio::MappingName());
    if (!mapping_) return false;

    mappedView_ = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, layout_.totalBytes);
    if (!mappedView_) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return false;
    }

    readyEvent_ = CreateEventW(nullptr, FALSE, FALSE, vasio::ReadyEventName());
    if (!readyEvent_) {
        UnmapViewOfFile(mappedView_);
        mappedView_ = nullptr;
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return false;
    }

    auto* base = static_cast<uint8_t*>(mappedView_);
    control_ = reinterpret_cast<vasio::SharedControlBlock*>(base);
    ringHeaders_ =
        reinterpret_cast<vasio::ChannelRingHeader*>(base + layout_.RingHeaderOffset());
    ringData_ = reinterpret_cast<float*>(base + layout_.RingDataOffset());

    // CreateFileMapping が新規作成した場合(GetLastError() != ERROR_ALREADY_EXISTS)は
    // control_ 内の atomic/POD が未初期化のゼロクリアされたページなので、明示的に
    // プレースメント new でコンストラクタを走らせる(std::atomic のデフォルト値・
    // ChannelRingHeader のデフォルト値を確定させるため)。既に engine プロセスが
    // 先に開いていた場合は上書きしない。
    if (GetLastError() != ERROR_ALREADY_EXISTS) {
        new (control_) vasio::SharedControlBlock();
        for (int i = 0; i < 2 * vasio::kMaxChannels; ++i) new (&ringHeaders_[i]) vasio::ChannelRingHeader();
        control_->ringCapacityFrames = kRingCapacityFrames;
    }

    return true;
}

void SluiceVasioDriver::DisconnectSharedMemory() {
    if (readyEvent_) {
        CloseHandle(readyEvent_);
        readyEvent_ = nullptr;
    }
    if (mappedView_) {
        UnmapViewOfFile(mappedView_);
        mappedView_ = nullptr;
    }
    if (mapping_) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
    control_ = nullptr;
    ringHeaders_ = nullptr;
    ringData_ = nullptr;
}

// ===========================================================================
// RT ワーカースレッド(実装ガイド §8.1 手順4・手順5)
// ===========================================================================

void SluiceVasioDriver::WorkerThreadMain() {
    constexpr DWORD kReadyTimeoutMs = 50;  // エンジン未接続時でも DAW を待たせすぎない
    HANDLE waitHandles[2] = {readyEvent_, stopEvent_};

    while (workerRunning_.load(std::memory_order_acquire)) {
        DWORD wait = readyEvent_
                          ? WaitForMultipleObjects(2, waitHandles, FALSE, kReadyTimeoutMs)
                          : (Sleep(kReadyTimeoutMs), WAIT_TIMEOUT);
        if (wait == WAIT_OBJECT_0 + 1) break;  // stopEvent_

        if (wait == WAIT_TIMEOUT && control_) {
            control_->engineTimeoutCount.fetch_add(1, std::memory_order_relaxed);
        }
        PumpOneBuffer();
    }
}

void SluiceVasioDriver::PumpOneBuffer() {
    if (!started_ || !callbacks_) return;

    const bool engineConnected =
        control_ &&
        control_->connectionState.load(std::memory_order_acquire) !=
            static_cast<uint32_t>(vasio::ConnectionState::Disconnected);

    // resetPending は「次のバッファ交換の直前」に通知する(実装ガイド §8.1 手順6:
    // 「エンジン側でレートやバッファサイズが変わったら vasio が DAW にリセットを
    // 要求する」)。一度通知したら resetNotified_ で多重送出を防ぐ。
    if (engineConnected &&
        control_->connectionState.load(std::memory_order_acquire) ==
            static_cast<uint32_t>(vasio::ConnectionState::ResetPending)) {
        if (!resetNotified_ && callbacks_->asioMessage) {
            callbacks_->asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
            resetNotified_ = true;
            // 送出済みを示すため Connected へ戻す(vasio_driver.h コメント参照)。
            // engine 側は「ResetPending を書いた後、値が Connected に戻ったら
            // 送出済み」と解釈できる(engine 側コンシューマは本フェーズ未実装だが、
            // 将来の実装がこの単純な合意に乗れるよう、ここで確定させておく)。
            control_->connectionState.store(static_cast<uint32_t>(vasio::ConnectionState::Connected),
                                             std::memory_order_release);
        }
    } else {
        resetNotified_ = false;
    }

    // FromEngine (エンジン→DAW、ASIO 的には「入力」) をリングから読み出す。
    for (long i = 0; i < activeInputs_; ++i) {
        float* dst = inputBuffers_[i] + (toggle_ ? blockFrames_ : 0);
        uint32_t got = 0;
        if (engineConnected) {
            const int channelIndex = vasio::kMaxChannels + static_cast<int>(inMap_[i]);
            got = vasio::RingRead(ringHeaders_[channelIndex],
                                   ringData_ + static_cast<size_t>(channelIndex) *
                                                   control_->ringCapacityFrames,
                                   control_->ringCapacityFrames, dst,
                                   static_cast<uint32_t>(blockFrames_));
        }
        for (uint32_t f = got; f < static_cast<uint32_t>(blockFrames_); ++f) dst[f] = 0.0f;  // 無音で埋める
    }

    // ToEngine (DAW→エンジン、ASIO 的には「出力」) をリングへ書き出す。
    for (long i = 0; i < activeOutputs_; ++i) {
        const float* src = outputBuffers_[i] + (toggle_ ? blockFrames_ : 0);
        if (engineConnected) {
            const int channelIndex = static_cast<int>(outMap_[i]);
            vasio::RingWrite(ringHeaders_[channelIndex],
                              ringData_ + static_cast<size_t>(channelIndex) *
                                              control_->ringCapacityFrames,
                              control_->ringCapacityFrames, src,
                              static_cast<uint32_t>(blockFrames_));
        }
        // engine 未接続時は書き込みを単に破棄する(DAW の出力を録らせる相手がいない)。
    }

    if (control_) control_->bufferSwitchCount.fetch_add(1, std::memory_order_relaxed);

    callbacks_->bufferSwitch(toggle_, ASIOFalse);
    toggle_ = toggle_ ? 0 : 1;
}

// ===========================================================================
// IASIO 実装
// ===========================================================================

ASIOBool SluiceVasioDriver::init(void* /*sysHandle*/) {
    if (active_) return ASIOTrue;
    std::strcpy(errorMessage_, "Sluice Virtual ASIO: init failed");

    // 共有メモリへの接続に失敗しても init 自体は成功させる(実装ガイド §8.1
    // 手順5の精神: エンジンが後から起動するケースも正常系として扱う。
    // ConnectSharedMemory の再試行は WorkerThreadMain の各ループでは行わず、
    // 次回 init/start の呼び直しに委ねる — 最小実装としての割り切り)。
    ConnectSharedMemory();

    active_ = true;
    return ASIOTrue;
}

void SluiceVasioDriver::getDriverName(char* name) { std::strcpy(name, "Sluice Virtual ASIO"); }

long SluiceVasioDriver::getDriverVersion() { return 0x00000001L; }

void SluiceVasioDriver::getErrorMessage(char* string) { std::strcpy(string, errorMessage_); }

ASIOError SluiceVasioDriver::start() {
    if (!callbacks_) return ASE_NotPresent;
    started_ = false;
    toggle_ = 0;
    resetNotified_ = false;

    workerRunning_.store(true, std::memory_order_release);
    ResetEvent(stopEvent_);
    worker_ = std::thread([this] { WorkerThreadMain(); });

    started_ = true;
    return ASE_OK;
}

ASIOError SluiceVasioDriver::stop() {
    started_ = false;
    if (workerRunning_.exchange(false)) {
        if (stopEvent_) SetEvent(stopEvent_);
        if (worker_.joinable()) worker_.join();
    }
    return ASE_OK;
}

ASIOError SluiceVasioDriver::getChannels(long* numInputChannels, long* numOutputChannels) {
    *numInputChannels = vasio::kMaxChannels;   // FromEngine
    *numOutputChannels = vasio::kMaxChannels;  // ToEngine
    return ASE_OK;
}

ASIOError SluiceVasioDriver::getLatencies(long* inputLatency, long* outputLatency) {
    // 共有メモリのリング容量ぶんを上限として概算(実測は tools/latencybench の対象)。
    *inputLatency = blockFrames_;
    *outputLatency = blockFrames_ * 2;
    return ASE_OK;
}

ASIOError SluiceVasioDriver::getBufferSize(long* minSize, long* maxSize, long* preferredSize,
                                            long* granularity) {
    *minSize = 32;
    *maxSize = 4096;
    *preferredSize = 512;
    *granularity = -1;  // 2 の冪のみ許容
    return ASE_OK;
}

ASIOError SluiceVasioDriver::canSampleRate(ASIOSampleRate sampleRate) {
    return (sampleRate == 44100.0 || sampleRate == 48000.0 || sampleRate == 96000.0) ? ASE_OK
                                                                                      : ASE_NoClock;
}

ASIOError SluiceVasioDriver::getSampleRate(ASIOSampleRate* sampleRate) {
    *sampleRate = sampleRate_;
    return ASE_OK;
}

ASIOError SluiceVasioDriver::setSampleRate(ASIOSampleRate sampleRate) {
    if (canSampleRate(sampleRate) != ASE_OK) return ASE_NoClock;
    if (sampleRate != sampleRate_) {
        sampleRate_ = sampleRate;
        if (callbacks_ && callbacks_->sampleRateDidChange) callbacks_->sampleRateDidChange(sampleRate_);
    }
    return ASE_OK;
}

ASIOError SluiceVasioDriver::getClockSources(ASIOClockSource* clocks, long* numSources) {
    clocks->index = 0;
    clocks->associatedChannel = -1;
    clocks->associatedGroup = -1;
    clocks->isCurrentSource = ASIOTrue;
    std::strcpy(clocks->name, "Sluice Engine");
    *numSources = 1;
    return ASE_OK;
}

ASIOError SluiceVasioDriver::setClockSource(long reference) {
    return reference == 0 ? ASE_OK : ASE_NotPresent;
}

ASIOError SluiceVasioDriver::getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) {
    // 簡易実装: 経過ブロック数 × blockFrames_ を返す(timeInfo モード非対応)。
    const uint64_t pos = control_ ? control_->bufferSwitchCount.load(std::memory_order_relaxed) *
                                         static_cast<uint64_t>(blockFrames_)
                                   : 0;
    sPos->hi = static_cast<unsigned long>(pos >> 32);
    sPos->lo = static_cast<unsigned long>(pos & 0xffffffffu);
    tStamp->hi = tStamp->lo = 0;
    return ASE_OK;
}

ASIOError SluiceVasioDriver::getChannelInfo(ASIOChannelInfo* info) {
    if (info->channel < 0 || info->channel >= vasio::kMaxChannels) return ASE_InvalidParameter;
    info->type = ASIOSTFloat32LSB;
    info->channelGroup = 0;
    info->isActive = ASIOFalse;
    const long* map = info->isInput ? inMap_ : outMap_;
    const long active = info->isInput ? activeInputs_ : activeOutputs_;
    for (long i = 0; i < active; ++i) {
        if (map[i] == info->channel) {
            info->isActive = ASIOTrue;
            break;
        }
    }
    std::snprintf(info->name, sizeof(info->name), "Sluice %s %ld", info->isInput ? "In" : "Out",
                  info->channel + 1);
    return ASE_OK;
}

ASIOError SluiceVasioDriver::createBuffers(ASIOBufferInfo* bufferInfos, long numChannels,
                                            long bufferSize, ASIOCallbacks* callbacks) {
    activeInputs_ = 0;
    activeOutputs_ = 0;
    blockFrames_ = bufferSize;

    ASIOBufferInfo* info = bufferInfos;
    for (long i = 0; i < numChannels; ++i, ++info) {
        if (info->isInput) {
            if (info->channelNum < 0 || info->channelNum >= vasio::kMaxChannels ||
                activeInputs_ >= vasio::kMaxChannels) {
                disposeBuffers();
                return ASE_InvalidParameter;
            }
            inMap_[activeInputs_] = info->channelNum;
            inputBuffers_[activeInputs_] = new float[static_cast<size_t>(blockFrames_) * 2];
            info->buffers[0] = inputBuffers_[activeInputs_];
            info->buffers[1] = inputBuffers_[activeInputs_] + blockFrames_;
            ++activeInputs_;
        } else {
            if (info->channelNum < 0 || info->channelNum >= vasio::kMaxChannels ||
                activeOutputs_ >= vasio::kMaxChannels) {
                disposeBuffers();
                return ASE_InvalidParameter;
            }
            outMap_[activeOutputs_] = info->channelNum;
            outputBuffers_[activeOutputs_] = new float[static_cast<size_t>(blockFrames_) * 2];
            info->buffers[0] = outputBuffers_[activeOutputs_];
            info->buffers[1] = outputBuffers_[activeOutputs_] + blockFrames_;
            ++activeOutputs_;
        }
    }

    callbacks_ = callbacks;
    if (control_) control_->bufferSizeFrames = static_cast<uint32_t>(blockFrames_);
    return ASE_OK;
}

ASIOError SluiceVasioDriver::disposeBuffers() {
    stop();
    callbacks_ = nullptr;
    for (long i = 0; i < activeInputs_; ++i) delete[] inputBuffers_[i];
    activeInputs_ = 0;
    for (long i = 0; i < activeOutputs_; ++i) delete[] outputBuffers_[i];
    activeOutputs_ = 0;
    return ASE_OK;
}

ASIOError SluiceVasioDriver::controlPanel() { return ASE_NotPresent; }

ASIOError SluiceVasioDriver::future(long selector, void* /*opt*/) {
    switch (selector) {
        case kAsioCanTimeInfo:
            return ASE_NotPresent;  // timeInfo モード非対応(スコープ外)
        default:
            return ASE_NotPresent;
    }
}

ASIOError SluiceVasioDriver::outputReady() { return ASE_NotPresent; }
