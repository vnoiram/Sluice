// ks_device.cpp : DirectKS (Kernel Streaming) キャプチャ/レンダー実装
//                 (実装ガイド §6.1「実装概要」の6ステップに対応)
//
// 検証状況: この Linux/WSL 開発環境には実 KS 対応デバイス・実 Windows SDK
// ヘッダが無く、本ファイルはコンパイル・実機検証ができていない
// (README/engine の他ファイルと同じ制約。Windows Docker でのコンパイル
// 確認 → 実機での動作確認が必須)。KSSTREAM_HEADER 経由のストリーミング
// I/O は WDK サンプル(ksstream 等)や PortAudio の WDM-KS バックエンドで
// 使われている標準的な手法に基づくが、ドライバ実装によっては細部の調整が
// 必要になる可能性がある。

#include "device/ks_device.h"

// KSCATEGORY_AUDIO 等の GUID は DEFINE_GUIDSTRUCT マクロで宣言されており、
// INITGUID なしだと「宣言のみ」で未解決シンボルになる。wasapi_device.cpp の
// functiondiscoverykeys_devpkey.h と同じ理由でこのファイル内だけ実体を持たせる。
#define INITGUID
#include <ks.h>
#include <ksmedia.h>
#undef INITGUID

#include <algorithm>
#include <cstring>

namespace ks {

namespace {

// NTSTATUS の成否判定(下位互換のため NT_SUCCESS マクロには依存しない)。
inline bool NtOk(LONG status) { return status >= 0; }

// リング容量: 周期フレーム数の 16 倍を 2 の冪へ切り上げ(wasapi_device.cpp と同じ方針)
size_t RingCapacityFor(UINT32 periodFrames) {
    size_t frames = (size_t)periodFrames * 16;
    size_t cap = 1;
    while (cap < frames) cap <<= 1;
    return cap;
}

// KSPROPERTY(GET 用)を組み立てて KsSynchronousDeviceControl で問い合わせる。
// pinId が -1 ならフィルタ全体向け(素の KSPROPERTY)、それ以外は KSP_PIN。
bool QueryKsProperty(HANDLE handle, const GUID& set, ULONG id, LONG pinId, void* out,
                      ULONG outSize, ULONG* bytesReturned) {
    if (pinId >= 0) {
        KSP_PIN prop{};
        prop.Property.Set = set;
        prop.Property.Id = id;
        prop.Property.Flags = KSPROPERTY_TYPE_GET;
        prop.PinId = (ULONG)pinId;
        return NtOk(KsSynchronousDeviceControl(handle, IOCTL_KS_PROPERTY, &prop, sizeof(prop),
                                                out, outSize, bytesReturned));
    }
    KSPROPERTY prop{};
    prop.Set = set;
    prop.Id = id;
    prop.Flags = KSPROPERTY_TYPE_GET;
    return NtOk(KsSynchronousDeviceControl(handle, IOCTL_KS_PROPERTY, &prop, sizeof(prop), out,
                                            outSize, bytesReturned));
}

}  // namespace

// ===========================================================================
// デバイス列挙(実装ガイド §6.1 手順1)
// ===========================================================================
std::vector<KsDeviceInfo> EnumerateKsAudioDevices() {
    std::vector<KsDeviceInfo> result;

    HDEVINFO devInfo = SetupDiGetClassDevsW(&KSCATEGORY_AUDIO, nullptr, nullptr,
                                             DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (devInfo == INVALID_HANDLE_VALUE) return result;

    SP_DEVICE_INTERFACE_DATA ifData{};
    ifData.cbSize = sizeof(ifData);
    for (DWORD index = 0;
         SetupDiEnumDeviceInterfaces(devInfo, nullptr, &KSCATEGORY_AUDIO, index, &ifData);
         ++index) {
        DWORD detailSize = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, nullptr, 0, &detailSize, nullptr);
        if (detailSize == 0) continue;

        std::vector<uint8_t> buf(detailSize);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        SP_DEVINFO_DATA devData{};
        devData.cbSize = sizeof(devData);
        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, detailSize, nullptr,
                                               &devData))
            continue;

        KsDeviceInfo info;
        info.symbolicLink = detail->DevicePath;

        // フレンドリ名(無ければデバイス説明、それも無ければシンボリックリンクで代用)。
        wchar_t nameBuf[256] = {};
        DWORD nameSize = 0;
        if (SetupDiGetDeviceRegistryPropertyW(devInfo, &devData, SPDRP_FRIENDLYNAME, nullptr,
                                               (PBYTE)nameBuf, sizeof(nameBuf), &nameSize) ||
            SetupDiGetDeviceRegistryPropertyW(devInfo, &devData, SPDRP_DEVICEDESC, nullptr,
                                               (PBYTE)nameBuf, sizeof(nameBuf), &nameSize)) {
            info.friendlyName = nameBuf;
        } else {
            info.friendlyName = info.symbolicLink;
        }

        result.push_back(std::move(info));
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return result;
}

// ===========================================================================
// 構築/破棄
// ===========================================================================
KsDevice::KsDevice(KsDeviceInfo info, bool isCapture)
    : info_(std::move(info)), isCapture_(isCapture) {
    currentPinState_ = KSSTATE_STOP;
}

// ===========================================================================
// 能力プローブ(実装ガイド §6.2)
// ===========================================================================
DeviceCaps KsDevice::Probe(double /*sampleRate*/) {
    DeviceCaps caps;
    caps.recommendedLane = Lane::RT;  // KS は常に RT Lane 候補

    if (pinHandle_ != INVALID_HANDLE_VALUE) {
        // 既に Open 済み。ピンは排他的なので新規にフィルタを開き直さず、
        // 現在の周期をそのまま返す。
        caps.minPeriodFrames = caps.defaultPeriodFrames = periodFrames_;
        caps.supports64 = (periodFrames_ == 64);
        return caps;
    }

    HANDLE filter = CreateFileW(info_.symbolicLink.c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                 nullptr);
    if (filter == INVALID_HANDLE_VALUE) return caps;

    ULONG pinCount = 0, bytesReturned = 0;
    if (QueryKsProperty(filter, KSPROPSETID_Pin, KSPROPERTY_PIN_CTYPES, -1, &pinCount,
                        sizeof(pinCount), &bytesReturned) &&
        pinCount > 0) {
        const KSPIN_DATAFLOW wantFlow = isCapture_ ? KSPIN_DATAFLOW_OUT : KSPIN_DATAFLOW_IN;
        for (ULONG pin = 0; pin < pinCount; ++pin) {
            KSPIN_DATAFLOW flow{};
            if (QueryKsProperty(filter, KSPROPSETID_Pin, KSPROPERTY_PIN_DATAFLOW, (LONG)pin,
                                &flow, sizeof(flow), &bytesReturned) &&
                flow == wantFlow) {
                // 要求方向のピンが存在する。素の KS ストリーミング I/O は
                // 任意のブロックサイズを ReadFile/WriteFile 単位で扱える
                // ため、64 サンプルも対応可能として報告する(WaveRT 非対応
                // ゆえの実効レイテンシは tools/latencybench の実測課題)。
                caps.supports64 = true;
                caps.defaultPeriodFrames = 512;
                break;
            }
        }
    }

    CloseHandle(filter);
    return caps;
}

// ===========================================================================
// フィルタのオープン(実装ガイド §6.1 手順2)
// ===========================================================================
bool KsDevice::OpenFilter(std::wstring* errorOut) {
    filterHandle_ =
        CreateFileW(info_.symbolicLink.c_str(), GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (filterHandle_ == INVALID_HANDLE_VALUE) {
        if (errorOut) *errorOut = L"CreateFile(filter) failed";
        return false;
    }
    return true;
}

// ===========================================================================
// ピン探索・生成(実装ガイド §6.1 手順3〜4)
// ===========================================================================
bool KsDevice::FindAndCreatePin(const DeviceStreamConfig& config, std::wstring* errorOut) {
    // 手順3: KSPROPERTY_PIN_CTYPES でピン総数を取得し、各ピンの DATAFLOW を見る。
    ULONG pinCount = 0, bytesReturned = 0;
    if (!QueryKsProperty(filterHandle_, KSPROPSETID_Pin, KSPROPERTY_PIN_CTYPES, -1, &pinCount,
                          sizeof(pinCount), &bytesReturned) ||
        pinCount == 0) {
        if (errorOut) *errorOut = L"KSPROPERTY_PIN_CTYPES failed or no pins";
        return false;
    }

    const KSPIN_DATAFLOW wantFlow = isCapture_ ? KSPIN_DATAFLOW_OUT : KSPIN_DATAFLOW_IN;
    bool found = false;

    for (ULONG pin = 0; pin < pinCount && !found; ++pin) {
        KSPIN_DATAFLOW flow{};
        if (!QueryKsProperty(filterHandle_, KSPROPSETID_Pin, KSPROPERTY_PIN_DATAFLOW, (LONG)pin,
                              &flow, sizeof(flow), &bytesReturned))
            continue;
        if (flow != wantFlow) continue;

        // 手順3続き: KSPROPERTY_PIN_DATARANGES(可変長)。まずサイズ問い合わせ。
        KSP_PIN prop{};
        prop.Property.Set = KSPROPSETID_Pin;
        prop.Property.Id = KSPROPERTY_PIN_DATARANGES;
        prop.Property.Flags = KSPROPERTY_TYPE_GET;
        prop.PinId = pin;
        ULONG neededSize = 0;
        KsSynchronousDeviceControl(filterHandle_, IOCTL_KS_PROPERTY, &prop, sizeof(prop), nullptr,
                                    0, &neededSize);
        if (neededSize == 0) continue;

        std::vector<uint8_t> rangesBuf(neededSize);
        if (!NtOk(KsSynchronousDeviceControl(filterHandle_, IOCTL_KS_PROPERTY, &prop, sizeof(prop),
                                              rangesBuf.data(), neededSize, &bytesReturned)))
            continue;

        // KSMULTIPLE_ITEM ヘッダの直後に KSDATARANGE(可変長)が Count 個並ぶ。
        auto* multi = reinterpret_cast<KSMULTIPLE_ITEM*>(rangesBuf.data());
        auto* cursor = reinterpret_cast<uint8_t*>(multi + 1);
        bool acceptsFloatOrPcm = false;
        for (ULONG i = 0; i < multi->Count; ++i) {
            auto* range = reinterpret_cast<KSDATARANGE*>(cursor);
            if (range->MajorFormat == KSDATAFORMAT_TYPE_AUDIO &&
                (range->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT ||
                 range->SubFormat == KSDATAFORMAT_SUBTYPE_PCM)) {
                acceptsFloatOrPcm = true;
                break;
            }
            cursor += range->FormatSize;
        }
        if (!acceptsFloatOrPcm) continue;

        // 手順4: 要求フォーマットを組み立てて KsCreatePin。まず Float32 で試し、
        // 拒否されたら 16bit PCM にフォールバックする。
        struct ConnectRequest {
            KSPIN_CONNECT connect;
            KSDATAFORMAT_WAVEFORMATEX format;
        };

        auto tryCreate = [&](bool asFloat) -> bool {
            ConnectRequest req{};
            req.connect.Interface.Set = KSINTERFACESETID_Standard;
            req.connect.Interface.Id = KSINTERFACE_STANDARD_STREAMING;
            req.connect.Interface.Flags = 0;
            req.connect.Medium.Set = KSMEDIUMSETID_Standard;
            req.connect.Medium.Id = KSMEDIUM_STANDARD_DEVIO;
            req.connect.Medium.Flags = 0;
            req.connect.PinId = pin;
            req.connect.PinToHandle = nullptr;
            req.connect.Priority.PriorityClass = KSPRIORITY_NORMAL;
            req.connect.Priority.PrioritySubClass = 1;

            WAVEFORMATEX& wfx = req.format.WaveFormatEx;
            wfx.wFormatTag = asFloat ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
            wfx.nChannels = (WORD)config.channels;
            wfx.nSamplesPerSec = (DWORD)config.sampleRate;
            wfx.wBitsPerSample = asFloat ? 32 : 16;
            wfx.nBlockAlign = (WORD)(wfx.nChannels * wfx.wBitsPerSample / 8);
            wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
            wfx.cbSize = 0;

            req.format.DataFormat.FormatSize = sizeof(KSDATAFORMAT_WAVEFORMATEX);
            req.format.DataFormat.Flags = 0;
            req.format.DataFormat.SampleSize = wfx.nBlockAlign;
            req.format.DataFormat.Reserved = 0;
            req.format.DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
            req.format.DataFormat.SubFormat =
                asFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
            req.format.DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;

            HANDLE h = INVALID_HANDLE_VALUE;
            if (!NtOk(KsCreatePin(filterHandle_, &req.connect, GENERIC_READ | GENERIC_WRITE, &h)))
                return false;

            pinHandle_ = h;
            pinId_ = pin;
            channels_ = config.channels;
            sampleRate_ = config.sampleRate;
            formatIsFloat_ = asFloat;
            bitsPerSample_ = asFloat ? 32 : 16;
            return true;
        };

        found = tryCreate(true) || tryCreate(false);
    }

    if (!found) {
        if (errorOut) *errorOut = L"no matching KS pin found for requested format";
        return false;
    }
    return true;
}

// ===========================================================================
// ピン状態遷移(実装ガイド §6.1 手順6)
//   KSSTATE_STOP <-> ACQUIRE <-> PAUSE <-> RUN は1段階ずつ遷移させる。
// ===========================================================================
bool KsDevice::SetPinState(ULONG state) {
    if (pinHandle_ == INVALID_HANDLE_VALUE) return false;

    auto stepTo = [&](ULONG target) -> bool {
        KSPROPERTY prop{};
        prop.Set = KSPROPSETID_Connection;
        prop.Id = KSPROPERTY_CONNECTION_STATE;
        prop.Flags = KSPROPERTY_TYPE_SET;
        ULONG bytesReturned = 0;
        KSSTATE value = (KSSTATE)target;
        if (!NtOk(KsSynchronousDeviceControl(pinHandle_, IOCTL_KS_PROPERTY, &prop, sizeof(prop),
                                              &value, sizeof(value), &bytesReturned)))
            return false;
        currentPinState_ = target;
        return true;
    };

    // 現在値から目標値まで、STOP(0) < ACQUIRE(1) < PAUSE(2) < RUN(3) の順に
    // 1 段階ずつ進める/戻す。
    while (currentPinState_ != state) {
        ULONG next = currentPinState_ < state ? currentPinState_ + 1 : currentPinState_ - 1;
        if (!stepTo(next)) return false;
    }
    return true;
}

// ===========================================================================
// IAudioDevice 契約
// ===========================================================================
bool KsDevice::Open(const DeviceStreamConfig& config, std::wstring* errorOut) {
    auto fail = [&](const wchar_t* msg) {
        if (errorOut) *errorOut = msg;
        Close();
        return false;
    };

    if (!OpenFilter(errorOut)) return false;
    if (!FindAndCreatePin(config, errorOut)) return fail(L"pin negotiation failed");

    periodFrames_ =
        config.preferredBufferFrames > 0 ? (UINT32)config.preferredBufferFrames : 512;

    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) return fail(L"CreateEvent failed");

    const int bytesPerSample = bitsPerSample_ / 8;
    const size_t frameBytes = (size_t)bytesPerSample * channels_;
    streamBuffer_.assign(sizeof(KSSTREAM_HEADER) + (size_t)periodFrames_ * frameBytes, 0);

    rings_.clear();
    rings_.reserve((size_t)channels_);
    const size_t cap = RingCapacityFor(periodFrames_);
    for (int c = 0; c < channels_; ++c) rings_.push_back(std::make_unique<SpscRing<float>>(cap));
    channelScratch_.assign((size_t)channels_ * periodFrames_, 0.0f);

    // ACQUIRE まで進めておく(実際の RUN は Start() で行う)。
    if (!SetPinState(KSSTATE_ACQUIRE) || !SetPinState(KSSTATE_PAUSE))
        return fail(L"pin state transition (ACQUIRE/PAUSE) failed");

    latencySeconds_ = (double)periodFrames_ / sampleRate_;
    return true;
}

void KsDevice::Start() {
    if (pinHandle_ == INVALID_HANDLE_VALUE || running_.load()) return;
    if (!SetPinState(KSSTATE_RUN)) return;
    running_.store(true);
    ResetEvent(stopEvent_);
    thread_ = std::thread(&KsDevice::ThreadMain, this);
}

void KsDevice::Stop() {
    if (running_.exchange(false)) {
        if (stopEvent_) SetEvent(stopEvent_);
        if (thread_.joinable()) thread_.join();
    }
    if (pinHandle_ != INVALID_HANDLE_VALUE) SetPinState(KSSTATE_PAUSE);
}

void KsDevice::Close() {
    Stop();
    if (pinHandle_ != INVALID_HANDLE_VALUE) {
        SetPinState(KSSTATE_STOP);
        CloseHandle(pinHandle_);
        pinHandle_ = INVALID_HANDLE_VALUE;
    }
    if (filterHandle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(filterHandle_);
        filterHandle_ = INVALID_HANDLE_VALUE;
    }
    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
    rings_.clear();
    channelScratch_.clear();
    streamBuffer_.clear();
    channels_ = 0;
    periodFrames_ = 0;
}

SpscRing<float>* KsDevice::CaptureRing(int ch) {
    if (!isCapture_ || ch < 0 || ch >= (int)rings_.size()) return nullptr;
    return rings_[(size_t)ch].get();
}
SpscRing<float>* KsDevice::RenderRing(int ch) {
    if (isCapture_ || ch < 0 || ch >= (int)rings_.size()) return nullptr;
    return rings_[(size_t)ch].get();
}

DeviceStatus KsDevice::Status() const {
    DeviceStatus s;
    s.callbackCount = cbCount_.load(std::memory_order_relaxed);
    s.underrunCount = underrunCount_.load(std::memory_order_relaxed);
    s.overrunCount = overrunCount_.load(std::memory_order_relaxed);
    s.bufferSizeFrames = (long)periodFrames_;
    s.effectiveLatencySeconds = latencySeconds_;
    s.resetRequested = resetRequested_.load(std::memory_order_relaxed);
    s.lane = Lane::RT;  // 実装ガイド §6.2: KS は常に RT Lane
    return s;
}

// ===========================================================================
// RT(専用スレッド)側: 素の KS ストリーミング I/O
// ===========================================================================
void KsDevice::ThreadMain() {
    while (running_.load(std::memory_order_relaxed)) {
        if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) break;

        cbCount_.fetch_add(1, std::memory_order_relaxed);
        if (isCapture_) {
            ProcessOneCapture();
        } else {
            ProcessOneRender();
        }
    }
}

void KsDevice::ProcessOneCapture() {
    const int bytesPerSample = bitsPerSample_ / 8;
    const size_t frameBytes = (size_t)bytesPerSample * channels_;
    auto* header = reinterpret_cast<KSSTREAM_HEADER*>(streamBuffer_.data());
    uint8_t* data = streamBuffer_.data() + sizeof(KSSTREAM_HEADER);

    std::memset(header, 0, sizeof(KSSTREAM_HEADER));
    header->Size = sizeof(KSSTREAM_HEADER);
    header->Data = data;
    header->FrameExtent = (ULONG)(periodFrames_ * frameBytes);

    DWORD bytesRead = 0;
    if (!ReadFile(pinHandle_, streamBuffer_.data(), (DWORD)streamBuffer_.size(), &bytesRead,
                   nullptr))
        return;

    const UINT32 gotFrames = (UINT32)(bytesRead > sizeof(KSSTREAM_HEADER)
                                           ? (bytesRead - sizeof(KSSTREAM_HEADER)) / frameBytes
                                           : 0);

    // インターリーブ → プレーナ(wasapi_device.cpp と同じ変換方針)
    for (int c = 0; c < channels_; ++c) {
        float* scratch = channelScratch_.data() + (size_t)c * periodFrames_;
        for (UINT32 f = 0; f < gotFrames; ++f) {
            const uint8_t* p = data + (size_t)f * frameBytes + (size_t)c * bytesPerSample;
            float v;
            if (formatIsFloat_) {
                std::memcpy(&v, p, 4);
            } else {
                int16_t s;
                std::memcpy(&s, p, 2);
                v = s / 32768.0f;
            }
            scratch[f] = v;
        }
        size_t written = rings_[(size_t)c]->Write(scratch, gotFrames);
        if (written < gotFrames) overrunCount_.fetch_add(1, std::memory_order_relaxed);
    }

    if (blockCallback_) blockCallback_((int)gotFrames);
}

void KsDevice::ProcessOneRender() {
    const int bytesPerSample = bitsPerSample_ / 8;
    const size_t frameBytes = (size_t)bytesPerSample * channels_;

    if (blockCallback_) blockCallback_((int)periodFrames_);

    auto* header = reinterpret_cast<KSSTREAM_HEADER*>(streamBuffer_.data());
    uint8_t* data = streamBuffer_.data() + sizeof(KSSTREAM_HEADER);

    for (int c = 0; c < channels_; ++c) {
        float* scratch = channelScratch_.data() + (size_t)c * periodFrames_;
        size_t got = rings_[(size_t)c]->Read(scratch, periodFrames_);
        if (got < periodFrames_) {
            underrunCount_.fetch_add(1, std::memory_order_relaxed);
            std::fill(scratch + got, scratch + periodFrames_, 0.0f);
        }
        for (UINT32 f = 0; f < periodFrames_; ++f) {
            uint8_t* p = data + (size_t)f * frameBytes + (size_t)c * bytesPerSample;
            if (formatIsFloat_) {
                std::memcpy(p, &scratch[f], 4);
            } else {
                float v = scratch[f] > 1.0f ? 1.0f : (scratch[f] < -1.0f ? -1.0f : scratch[f]);
                int16_t s = (int16_t)(v * 32767.0f);
                std::memcpy(p, &s, 2);
            }
        }
    }

    std::memset(header, 0, sizeof(KSSTREAM_HEADER));
    header->Size = sizeof(KSSTREAM_HEADER);
    header->Data = data;
    header->FrameExtent = (ULONG)(periodFrames_ * frameBytes);
    header->DataUsed = (ULONG)(periodFrames_ * frameBytes);

    DWORD bytesWritten = 0;
    WriteFile(pinHandle_, streamBuffer_.data(), (DWORD)streamBuffer_.size(), &bytesWritten,
              nullptr);
}

}  // namespace ks
