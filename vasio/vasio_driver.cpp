// vasio_driver.cpp : 仮想 ASIO ドライバ本体の実装(実装ガイド §8.1)
//
// driver/asiosample/asiosmpl.cpp(Steinberg 提供のサンプルドライバ、当初の
// 実装時に構成の参考にした)と同じ役回りだが、ASIO SDK は使わない。
// 「テスト波形を生成する」代わりに「共有メモリ越しに engine プロセスと
// データを交換する」のが本質的な違い。COM ボイラープレート
// (単一 CLSID 用クラスファクトリ、DllGetClassObject/DllCanUnloadNow)は
// asio-abi/com_server.h の独自実装を使う。asio-abi/README.md 参照。

#include "vasio_driver.h"

#include <cstdio>
#include <cstring>

#include "../asio-abi/asio_registry.h"
#include "shared_security.h"

namespace {

// gap 11: 複数インスタンス対応。vasio.dll は DAW プロセス内にロードされ
// CLI 引数を持たないため、instanceId は環境変数から読む(engine 側は
// main.cpp の --vasio-instance で明示指定する、vasio_bridge_device.cpp
// 参照)。未設定なら既定の "0"(導入前の固定名と完全に後方互換)。
// DAW を起動する側が SLUICE_VASIO_INSTANCE を設定して、複数の DAW
// プロセスをそれぞれ別の engine 側インスタンスへ振り分ける想定。
std::wstring ResolveInstanceIdFromEnvironment() {
    wchar_t buf[64] = {};
    const DWORD len = GetEnvironmentVariableW(L"SLUICE_VASIO_INSTANCE", buf, 64);
    if (len == 0 || len >= 64) return L"0";
    return buf;
}

}  // namespace

// ===========================================================================
// CLSID / COM エントリポイント
// ===========================================================================

// {A1B2C3D4-1234-4E56-8F9A-0123456789AB}(開発用の仮 CLSID。vasio_driver.h 参照)
CLSID CLSID_SluiceVasio = {
    0xa1b2c3d4, 0x1234, 0x4e56, {0x8f, 0x9a, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab}};

IUnknown* SluiceVasioDriver::CreateInstance() { return new SluiceVasioDriver(); }

STDMETHODIMP SluiceVasioDriver::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    // ASIO の作法: 標準の IID ではなく、ドライバ自身の CLSID を IID として
    // 比較する(asio-abi/README.md、driver/asiosample/asiosmpl.cpp の前例と
    // 同じ非標準の慣習)。加えて素の IUnknown 問い合わせにも応答しておく
    // (一般的な COM クライアントからの同一性比較に対応するため)。
    if (riid == CLSID_SluiceVasio || riid == IID_IUnknown) {
        *ppv = static_cast<IASIO*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    return comserver::DllGetClassObjectImpl(rclsid, CLSID_SluiceVasio, riid, ppv,
                                            &SluiceVasioDriver::CreateInstance);
}

extern "C" HRESULT __stdcall DllCanUnloadNow() { return comserver::DllCanUnloadNowImpl(); }

// この DLL 自身のモジュールハンドル。DllRegisterServer が
// InProcServer32 に書き込む DLL パスを取得するために使う
// (GetModuleFileNameW に nullptr を渡すと「現在のプロセスの実行ファイル」の
// パスが返ってしまい、DAW にロードされた vasio.dll 自身のパスにはならない)。
HINSTANCE g_hinstDll = nullptr;

extern "C" BOOL WINAPI DllMain(HINSTANCE hinstDLL, ULONG reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hinstDll = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
    }
    return TRUE;
}

// ===========================================================================
// COM 登録/解除(実装ガイド §8.1 手順2)
//   HKCR\CLSID\{...}\InProcServer32 と HKLM\SOFTWARE\ASIO\<name> の両方を
//   asio-abi/asio_registry.h の RegisterAsioDriver/UnregisterAsioDriver に
//   任せる。DLL 名・表示名はここでしか出てこないので、ビルド成果物の実際の
//   ファイル名(vasio.dll)と一致させること。
// ===========================================================================

extern "C" HRESULT __stdcall DllRegisterServer() {
    wchar_t modulePath[MAX_PATH]{};
    // hinstDLL はこの DLL 自身のモジュールハンドル。nullptr を渡すと
    // GetModuleFileNameW は「現在のプロセスの実行ファイル」を返してしまう
    // ため、必ず DllMain で受け取ったハンドル相当(g_hinstDll)を使うこと。
    GetModuleFileNameW(g_hinstDll, modulePath, MAX_PATH);

    const long rc = asioabi::RegisterAsioDriver(CLSID_SluiceVasio, modulePath,
                                                L"Sluice Virtual ASIO", L"Sluice Virtual ASIO");
    if (rc != ERROR_SUCCESS) {
        wchar_t msg[128];
        swprintf_s(msg, L"Register Server failed! (%ld)", rc);
        MessageBoxW(nullptr, msg, L"Sluice Virtual ASIO", MB_OK);
        return E_FAIL;
    }
    return S_OK;
}

extern "C" HRESULT __stdcall DllUnregisterServer() {
    const long rc =
        asioabi::UnregisterAsioDriver(CLSID_SluiceVasio, L"Sluice Virtual ASIO");
    if (rc != ERROR_SUCCESS) {
        wchar_t msg[128];
        swprintf_s(msg, L"Unregister Server failed! (%ld)", rc);
        MessageBoxW(nullptr, msg, L"Sluice Virtual ASIO", MB_OK);
        return E_FAIL;
    }
    return S_OK;
}

// ===========================================================================
// 構築/破棄
// ===========================================================================

SluiceVasioDriver::SluiceVasioDriver() {
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
    // 値は shared_protocol.h の kDefaultRingCapacityFrames 1 箇所で管理する
    // (engine 側コンシューマと食い違うと ComputeLayout() の結果がずれる)。
    constexpr uint32_t kRingCapacityFrames = vasio::kDefaultRingCapacityFrames;
    layout_ = vasio::ComputeLayout(kRingCapacityFrames);

    const std::wstring instanceId = ResolveInstanceIdFromEnvironment();

    // 同一セッションの他プロセスが Local\SluiceVasio.<id> を先回りして
    // 作成/汚染できないよう、現在ユーザーのみアクセス可な DACL を付与する
    // (vasio_bridge_device.cpp 側も同じ shared_security.h で構築するため、
    // どちらが先に作成しても同じ制限になる)。
    vasio::CurrentUserOnlySecurityAttributes security;

    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, security.attributes(), PAGE_READWRITE, 0,
                                   static_cast<DWORD>(layout_.totalBytes),
                                   vasio::MappingName(instanceId).c_str());
    // CreateFileMappingW 直後でなければならない(次の Win32 呼び出しで
    // 上書きされるため)。以前はこの取得を省略しており、後続の
    // MapViewOfFile/CreateEventW が上書きした値で ERROR_ALREADY_EXISTS を
    // 誤判定するバグがあった。
    const DWORD createErr = GetLastError();
    if (!mapping_) return false;

    mappedView_ = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, layout_.totalBytes);
    if (!mappedView_) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return false;
    }

    readyEvent_ = CreateEventW(security.attributes(), FALSE, FALSE,
                                vasio::ReadyEventName(instanceId).c_str());
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

    // CreateFileMapping が新規作成した場合(createErr != ERROR_ALREADY_EXISTS)は
    // control_ 内の atomic/POD が未初期化のゼロクリアされたページなので、明示的に
    // プレースメント new でコンストラクタを走らせる(std::atomic のデフォルト値・
    // ChannelRingHeader のデフォルト値を確定させるため)。既に engine プロセスが
    // 先に開いていた場合は上書きしない。
    if (createErr != ERROR_ALREADY_EXISTS) {
        new (control_) vasio::SharedControlBlock();
        for (int i = 0; i < 2 * vasio::kMaxChannels; ++i) new (&ringHeaders_[i]) vasio::ChannelRingHeader();
        control_->ringCapacityFrames = kRingCapacityFrames;
    }

    // 相手側(engine)が先に接続していた場合、そのプロセスが書き込んだ
    // フィールドを使う前に検証する(悪意ある/破損したピアからの防御)。
    if (!vasio::ValidateControlBlock(*control_, kRingCapacityFrames)) {
        DisconnectSharedMemory();
        return false;
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

    // リング領域へアクセスしてよいのは、接続時に検証したレイアウトと
    // ringCapacityFrames が一致している場合のみ(相手が接続後に不正な値へ
    // 書き換えた場合の防御。cap==0 のゼロ除算も RingRead/RingWrite 側で
    // ガードしているが、ここではマッピング済み領域外へのオフセット計算
    // 自体を避ける)。
    const bool ringUsable =
        engineConnected && control_->ringCapacityFrames == vasio::kDefaultRingCapacityFrames;

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
        if (ringUsable) {
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
        if (ringUsable) {
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
