// process_loopback_device.cpp : プロセス別ループバックキャプチャ実装
#include "device/process_loopback_device.h"

#include <audioclientactivationparams.h>
#include <avrt.h>

#include <algorithm>
#include <cstring>

namespace wasapi {

namespace {
size_t RingCapacityFor(UINT32 periodFrames) {
    size_t frames = (size_t)periodFrames * 16;
    size_t cap = 1;
    while (cap < frames) cap <<= 1;
    return cap;
}
}  // namespace

// ===========================================================================
// IActivateAudioInterfaceCompletionHandler
//
// ActivateAudioInterfaceAsync は非同期 API で、結果はこのハンドラの
// ActivateCompleted 経由でしか受け取れない。ここでは単純にイベントで
// 同期させ、呼び出し元スレッドは Wait() でブロックして待つ
// (アクティベーションは起動時の 1 回きりなので RT ではない。ブロッキング
// で問題ない)。
// ===========================================================================
class ProcessLoopbackDevice::ActivationHandler
    : public IActivateAudioInterfaceCompletionHandler {
public:
    ActivationHandler() : doneEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~ActivationHandler() { if (doneEvent_) CloseHandle(doneEvent_); }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = ref_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE ActivateCompleted(
        IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT activateHr = E_FAIL;
        IUnknown* unk = nullptr;
        op->GetActivateResult(&activateHr, &unk);
        activateHr_ = activateHr;
        if (SUCCEEDED(activateHr_) && unk) {
            unk->QueryInterface(__uuidof(IAudioClient),
                                reinterpret_cast<void**>(&audioClient_));
            unk->Release();
        }
        SetEvent(doneEvent_);
        return S_OK;
    }

    // 呼び出し元スレッドで完了を待ち、結果の IAudioClient(所有権譲渡)を返す。
    HRESULT Wait(IAudioClient** outClient) {
        WaitForSingleObject(doneEvent_, INFINITE);
        *outClient = audioClient_;
        audioClient_ = nullptr;
        return activateHr_;
    }

private:
    std::atomic<ULONG> ref_{1};
    HANDLE doneEvent_ = nullptr;
    HRESULT activateHr_ = E_FAIL;
    IAudioClient* audioClient_ = nullptr;
};

// ===========================================================================
// 構築 / Open / Start / Stop / Close
// ===========================================================================
ProcessLoopbackDevice::ProcessLoopbackDevice(DWORD targetPid, bool includeChildren)
    : targetPid_(targetPid), includeChildren_(includeChildren) {}

bool ProcessLoopbackDevice::Open(const DeviceStreamConfig& config, std::wstring* errorOut) {
    auto fail = [&](const wchar_t* msg) {
        if (errorOut) *errorOut = msg;
        Close();
        return false;
    };

    channels_ = config.channels;
    sampleRate_ = config.sampleRate;

    // 実装ガイド §5.3: フォーマットは自分で指定できる。float32 を要求
    // すれば以降の型変換が不要になる。
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = (WORD)channels_;
    format.nSamplesPerSec = (DWORD)sampleRate_;
    format.wBitsPerSample = 32;
    format.nBlockAlign = (WORD)(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    format.cbSize = 0;

    AUDIOCLIENT_ACTIVATION_PARAMS activationParams{};
    activationParams.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    activationParams.ProcessLoopbackParams.TargetProcessId = targetPid_;
    activationParams.ProcessLoopbackParams.ProcessLoopbackMode = includeChildren_
        ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE
        : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(activationParams);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&activationParams);

    auto* handler = new ActivationHandler();
    IActivateAudioInterfaceAsyncOperation* asyncOp = nullptr;
    HRESULT hr = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv,
        handler, &asyncOp);
    if (FAILED(hr)) {
        handler->Release();
        return fail(L"ActivateAudioInterfaceAsync failed (needs Windows 10 2004+)");
    }

    hr = handler->Wait(&audioClient_);
    if (asyncOp) asyncOp->Release();
    handler->Release();
    if (FAILED(hr) || !audioClient_)
        return fail(L"process loopback activation failed (invalid PID?)");

    // 10ms 周期を要求(100ns 単位: 1ms = 10,000)
    constexpr REFERENCE_TIME kBufferDuration = 10 * 10000;
    if (FAILED(audioClient_->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            kBufferDuration, 0, &format, nullptr)))
        return fail(L"IAudioClient Initialize failed");

    if (FAILED(audioClient_->GetBufferSize(&periodFrames_)))
        return fail(L"GetBufferSize failed");

    audioEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    stopEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!audioEvent_ || !stopEvent_) return fail(L"CreateEvent failed");
    if (FAILED(audioClient_->SetEventHandle(audioEvent_)))
        return fail(L"SetEventHandle failed");

    if (FAILED(audioClient_->GetService(__uuidof(IAudioCaptureClient),
                                        reinterpret_cast<void**>(&captureClient_))))
        return fail(L"GetService(IAudioCaptureClient) failed");

    REFERENCE_TIME latency = 0;
    if (SUCCEEDED(audioClient_->GetStreamLatency(&latency)))
        latencySeconds_ = (double)latency / 1.0e7;

    rings_.clear();
    rings_.reserve((size_t)channels_);
    const size_t cap = RingCapacityFor(periodFrames_);
    for (int c = 0; c < channels_; ++c)
        rings_.push_back(std::make_unique<SpscRing<float>>(cap));
    channelScratch_.assign((size_t)channels_ * periodFrames_, 0.0f);

    // PROCESS_QUERY_LIMITED_INFORMATION: 終了検知(GetExitCodeProcess)だけに
    // 使う最小権限。プロセスが既に存在しない場合は nullptr のままでよい
    // (TargetProcessAlive() は false を返す)。
    targetProcessHandle_ = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, targetPid_);

    return true;
}

void ProcessLoopbackDevice::Start() {
    if (!audioClient_ || running_.load()) return;
    if (FAILED(audioClient_->Start())) return;
    running_.store(true);
    thread_ = std::thread(&ProcessLoopbackDevice::ThreadMain, this);
}

void ProcessLoopbackDevice::Stop() {
    if (running_.exchange(false)) {
        if (stopEvent_) SetEvent(stopEvent_);
        if (thread_.joinable()) thread_.join();
    }
    if (audioClient_) audioClient_->Stop();
}

void ProcessLoopbackDevice::Close() {
    Stop();

    if (captureClient_) { captureClient_->Release(); captureClient_ = nullptr; }
    if (audioClient_)   { audioClient_->Release();   audioClient_   = nullptr; }
    if (targetProcessHandle_) { CloseHandle(targetProcessHandle_); targetProcessHandle_ = nullptr; }
    if (audioEvent_)    { CloseHandle(audioEvent_);   audioEvent_    = nullptr; }
    if (stopEvent_)     { CloseHandle(stopEvent_);    stopEvent_     = nullptr; }

    rings_.clear();
    channelScratch_.clear();
    channels_ = 0;
    periodFrames_ = 0;
}

// ===========================================================================
// IAudioDevice 契約
// ===========================================================================
SpscRing<float>* ProcessLoopbackDevice::CaptureRing(int ch) {
    if (ch < 0 || ch >= (int)rings_.size()) return nullptr;
    return rings_[(size_t)ch].get();
}

DeviceStatus ProcessLoopbackDevice::Status() const {
    DeviceStatus s;
    s.callbackCount = cbCount_.load(std::memory_order_relaxed);
    s.overrunCount = overrunCount_.load(std::memory_order_relaxed);
    s.bufferSizeFrames = (long)periodFrames_;
    s.effectiveLatencySeconds = latencySeconds_;
    s.resetRequested = resetRequested_.load(std::memory_order_relaxed);
    return s;
}

bool ProcessLoopbackDevice::TargetProcessAlive() const {
    if (!targetProcessHandle_) return false;
    DWORD code = 0;
    if (!GetExitCodeProcess(targetProcessHandle_, &code)) return false;
    return code == STILL_ACTIVE;
}

// ===========================================================================
// RT(専用スレッド)側
// ===========================================================================
void ProcessLoopbackDevice::ThreadMain() {
    const bool comInit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));

    DWORD taskIndex = 0;
    HANDLE avrtHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    HANDLE waitHandles[2] = { stopEvent_, audioEvent_ };
    while (running_.load(std::memory_order_relaxed)) {
        DWORD wait = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) break;
        if (wait != WAIT_OBJECT_0 + 1) continue;

        cbCount_.fetch_add(1, std::memory_order_relaxed);
        ProcessOneCapture();

        // 対象プロセスの終了検知(実装ガイド §5.3: 自動再アタッチ推奨)。
        // 実際の再アタッチは呼び出し側が resetRequested を見て行う
        // (ASIO の kAsioResetRequest と同じ扱いに揃えている)。
        if (!TargetProcessAlive()) resetRequested_.store(true);
    }

    if (avrtHandle) AvRevertMmThreadCharacteristics(avrtHandle);
    if (comInit) CoUninitialize();
}

void ProcessLoopbackDevice::ProcessOneCapture() {
    UINT32 totalFrames = 0;
    UINT32 packetLength = 0;
    HRESULT hr = captureClient_->GetNextPacketSize(&packetLength);

    while (SUCCEEDED(hr) && packetLength != 0) {
        BYTE* data = nullptr;
        UINT32 numFrames = 0;
        DWORD flags = 0;
        hr = captureClient_->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr);
        if (FAILED(hr)) break;

        const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        const float* src = reinterpret_cast<const float*>(data);
        for (UINT32 f = 0; f < numFrames; ++f) {
            for (int c = 0; c < channels_; ++c) {
                channelScratch_[(size_t)c * periodFrames_ + f] =
                    silent ? 0.0f : src[(size_t)f * channels_ + c];
            }
        }
        for (int c = 0; c < channels_; ++c) {
            const float* chBuf = channelScratch_.data() + (size_t)c * periodFrames_;
            if (rings_[(size_t)c]->Write(chBuf, numFrames) < numFrames)
                overrunCount_.fetch_add(1, std::memory_order_relaxed);
        }

        captureClient_->ReleaseBuffer(numFrames);
        totalFrames += numFrames;
        hr = captureClient_->GetNextPacketSize(&packetLength);
    }

    if (blockCallback_) blockCallback_((int)totalFrames);
}

}  // namespace wasapi
