// wasapi_device.cpp : WASAPI 共有モードキャプチャ/レンダー実装
#include "device/wasapi_device.h"

#include "device/wasapi_period.h"

#include <avrt.h>
#include <propsys.h>
// PKEY_Device_FriendlyName 等は DEFINE_PROPERTYKEY マクロで宣言されており、
// INITGUID なしだと「宣言のみ」(実体は別ライブラリ頼み)になり未解決
// シンボルでリンクエラーになる。このファイル内だけで実体を持たせる。
#define INITGUID
#include <functiondiscoverykeys_devpkey.h>
#undef INITGUID

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace wasapi {

namespace {

// --- サンプル型変換ヘルパ(PCM 16/24/32bit ⇔ float) -----------------------
inline float PcmToFloat(const uint8_t* p, int bytesPerSample) {
    switch (bytesPerSample) {
    case 2: {
        int16_t v;
        std::memcpy(&v, p, 2);
        return v / 32768.0f;
    }
    case 3: {
        // Int24 の 3 バイトパック。符号拡張を忘れない(実装ガイド §11)。
        int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                              ((uint32_t)p[2] << 16));
        if (v & 0x00800000) v |= (int32_t)0xFF000000u;
        return v / 8388608.0f;  // 2^23
    }
    case 4: {
        int32_t v;
        std::memcpy(&v, p, 4);
        return v / 2147483648.0f;
    }
    default:
        return 0.0f;
    }
}

inline void FloatToPcm(float v, uint8_t* p, int bytesPerSample) {
    v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
    switch (bytesPerSample) {
    case 2: {
        int16_t s = (int16_t)(v * 32767.0f);
        std::memcpy(p, &s, 2);
        break;
    }
    case 3: {
        int32_t s = (int32_t)(v * 8388607.0f);
        p[0] = (uint8_t)(s & 0xFF);
        p[1] = (uint8_t)((s >> 8) & 0xFF);
        p[2] = (uint8_t)((s >> 16) & 0xFF);
        break;
    }
    case 4: {
        int32_t s = (int32_t)(v * 2147483647.0f);
        std::memcpy(p, &s, 4);
        break;
    }
    default:
        break;
    }
}

// リング容量: 周期フレーム数の 16 倍を 2 の冪へ切り上げ(asio_host.cpp と同じ方針)
size_t RingCapacityFor(UINT32 periodFrames) {
    size_t frames = (size_t)periodFrames * 16;
    size_t cap = 1;
    while (cap < frames) cap <<= 1;
    return cap;
}

std::wstring GetFriendlyName(IMMDevice* device) {
    IPropertyStore* store = nullptr;
    std::wstring name = L"(unknown)";
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &pv)) &&
            pv.vt == VT_LPWSTR && pv.pwszVal) {
            name = pv.pwszVal;
        }
        PropVariantClear(&pv);
        store->Release();
    }
    return name;
}

}  // namespace

// ===========================================================================
// 排他モード(実装ガイド §5.2.5)
// ===========================================================================
// SubFormat GUID は「先頭 4 バイト(Data1)=通常の wFormatTag 値、残りは
// 固定の PCM/IEEE_FLOAT サブタイプの定型サフィックス」という規約(このファイル
// 内の formatTag 判定と対称、ksmedia.h の KSDATAFORMAT_SUBTYPE_* 定数は
// 持ち込まずに済む)。
WAVEFORMATEXTENSIBLE WasapiDevice::BuildCandidateFormat(const DeviceStreamConfig& config,
                                                        bool floatFormat) {
    WAVEFORMATEXTENSIBLE fmt{};
    const WORD bits = floatFormat ? 32 : 16;
    const WORD blockAlign = (WORD)(config.channels * (bits / 8));

    fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt.Format.nChannels = (WORD)config.channels;
    fmt.Format.nSamplesPerSec = (DWORD)config.sampleRate;
    fmt.Format.wBitsPerSample = bits;
    fmt.Format.nBlockAlign = blockAlign;
    fmt.Format.nAvgBytesPerSec = fmt.Format.nSamplesPerSec * blockAlign;
    fmt.Format.cbSize = 22;  // sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)
    fmt.Samples.wValidBitsPerSample = bits;
    // チャンネルマスク: 2ch 以下の一般的な配置のみ扱う(3ch 以上は将来課題)。
    // SPEAKER_FRONT_LEFT/RIGHT/CENTER の値(ksmedia.h 由来だが定義場所に
    // 依存したくないためリテラルで書く。値自体は Win32 の固定 ABI)。
    constexpr DWORD kSpeakerFrontLeft = 0x1;
    constexpr DWORD kSpeakerFrontRight = 0x2;
    constexpr DWORD kSpeakerFrontCenter = 0x4;
    fmt.dwChannelMask = (config.channels >= 2) ? (kSpeakerFrontLeft | kSpeakerFrontRight)
                                               : kSpeakerFrontCenter;
    fmt.SubFormat.Data1 = floatFormat ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
    fmt.SubFormat.Data2 = 0x0000;
    fmt.SubFormat.Data3 = 0x0010;
    static const uint8_t kSubFormatSuffix[8] = {0x80, 0x00, 0x00, 0xaa,
                                                0x00, 0x38, 0x9b, 0x71};
    std::memcpy(fmt.SubFormat.Data4, kSubFormatSuffix, sizeof(kSubFormatSuffix));
    return fmt;
}

// ===========================================================================
// エンドポイント列挙
// ===========================================================================
std::vector<EndpointInfo> EnumerateEndpoints(bool isCapture) {
    std::vector<EndpointInfo> result;
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator))))
        return result;

    IMMDeviceCollection* collection = nullptr;
    HRESULT hr = enumerator->EnumAudioEndpoints(
        isCapture ? eCapture : eRender, DEVICE_STATE_ACTIVE, &collection);
    if (SUCCEEDED(hr) && collection) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* dev = nullptr;
            if (FAILED(collection->Item(i, &dev)) || !dev) continue;
            LPWSTR id = nullptr;
            if (SUCCEEDED(dev->GetId(&id)) && id) {
                EndpointInfo info;
                info.id = id;
                info.name = GetFriendlyName(dev);
                result.push_back(std::move(info));
                CoTaskMemFree(id);
            }
            dev->Release();
        }
        collection->Release();
    }
    enumerator->Release();
    return result;
}

// ===========================================================================
// IMMNotificationClient(ホットプラグ検知)
// ===========================================================================
class WasapiDevice::NotificationClient : public IMMNotificationClient {
public:
    NotificationClient(WasapiDevice* owner, std::wstring watchedId)
        : owner_(owner), watchedId_(std::move(watchedId)) {}

    // --- IUnknown ---
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
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    // --- IMMNotificationClient ---
    // 監視対象デバイス(endpointId 指定時)またはデフォルトデバイス
    // (未指定時)に関わる変化があれば resetRequested を立てる。
    // 実際の Close→Open による再構築は呼び出し側(main.cpp 相当)の
    // 監視ループが行う(ASIO の kAsioResetRequest と同じ扱い)。
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR deviceId, DWORD) override {
        if (Matches(deviceId)) owner_->RequestReset();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR deviceId) override {
        if (Matches(deviceId)) owner_->RequestReset();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole,
                                                     LPCWSTR) override {
        if (watchedId_.empty()) owner_->RequestReset();  // 既定デバイス追従時のみ
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR,
                                                     const PROPERTYKEY) override {
        return S_OK;
    }

private:
    bool Matches(LPCWSTR deviceId) const {
        return watchedId_.empty() || (deviceId != nullptr && watchedId_ == deviceId);
    }
    std::atomic<ULONG> ref_{1};
    WasapiDevice* owner_;
    std::wstring watchedId_;
};

// ===========================================================================
// 構築 / Open / Start / Stop / Close
// ===========================================================================
WasapiDevice::WasapiDevice(std::wstring endpointId, bool isCapture)
    : endpointId_(std::move(endpointId)), isCapture_(isCapture) {}

// ===========================================================================
// 周期問い合わせ(実装ガイド §5.2.1)。Probe()/Open() 共通ヘルパ。
// ===========================================================================
bool WasapiDevice::QuerySharedModePeriod(IAudioClient* client, WAVEFORMATEX* mixFormat,
                                          bool rawMode, UINT32* defaultPeriod,
                                          UINT32* fundamentalPeriod, UINT32* minPeriod,
                                          UINT32* maxPeriod) {
    bool ok = false;
    IAudioClient3* client3 = nullptr;
    if (SUCCEEDED(client->QueryInterface(__uuidof(IAudioClient3),
                                         reinterpret_cast<void**>(&client3)))) {
        // ① SetClientProperties を GetSharedModeEnginePeriod より先に呼ぶ
        //    (実装ガイド §5.2.1: 「呼び出し順序が重要。返る周期はこれに依存する」)。
        //    client3 は IAudioClient2 を継承しているため QI は共用できる。
        AudioClientProperties props{};
        props.cbSize = sizeof(props);
        props.bIsOffload = FALSE;
        props.eCategory = AudioCategory_Media;
        props.Options = rawMode ? AUDCLNT_STREAMOPTIONS_RAW : AUDCLNT_STREAMOPTIONS_NONE;
        client3->SetClientProperties(&props);

        // ② ミックスフォーマットは呼び出し側から渡されたものをそのまま使う
        //    (GetMixFormat と異なるレートで問い合わせるとエラーになるため、
        //    呼び出し側が既に取得済みの mixFormat を再利用する)。
        // ③ 周期の範囲を取得
        ok = SUCCEEDED(client3->GetSharedModeEnginePeriod(mixFormat, defaultPeriod,
                                                           fundamentalPeriod, minPeriod,
                                                           maxPeriod));
        client3->Release();
    }
    return ok;
}

// ===========================================================================
// 能力プローブ(実装ガイド §5.2.1)
// ===========================================================================
DeviceCaps WasapiDevice::Probe(double sampleRate) {
    DeviceCaps caps;

    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator))))
        return caps;

    IMMDevice* device = nullptr;
    HRESULT hr = endpointId_.empty()
        ? enumerator->GetDefaultAudioEndpoint(isCapture_ ? eCapture : eRender, eConsole, &device)
        : enumerator->GetDevice(endpointId_.c_str(), &device);
    if (FAILED(hr) || !device) {
        enumerator->Release();
        return caps;
    }

    IAudioClient* client = nullptr;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&client)))) {
        device->Release();
        enumerator->Release();
        return caps;
    }

    WAVEFORMATEX* mixFormat = nullptr;
    if (SUCCEEDED(client->GetMixFormat(&mixFormat)) && mixFormat) {
        // sampleRate 引数はプローブ対象レートの参考情報。GetMixFormat は
        // デバイス既定レートを返すため、決め打ちの置き換えはしない
        // (実装ガイド §5.2.4「GetMixFormat の結果に従う。決め打ちしない」)。
        (void)sampleRate;

        UINT32 defaultPeriod = 0, fundamentalPeriod = 0, minPeriod = 0, maxPeriod = 0;
        if (QuerySharedModePeriod(client, mixFormat, /*rawMode=*/false, &defaultPeriod,
                                   &fundamentalPeriod, &minPeriod, &maxPeriod)) {
            caps.minPeriodFrames = minPeriod;
            caps.fundamentalFrames = fundamentalPeriod;
            caps.defaultPeriodFrames = defaultPeriod;
            // 実装ガイド §5.2.1: 「fundamental の整数倍」制約を含めた判定。
            caps.supports64 = (64 >= minPeriod && 64 <= maxPeriod) &&
                               (fundamentalPeriod == 0 || 64 % fundamentalPeriod == 0);
            caps.recommendedLane = caps.supports64 ? Lane::RT : Lane::Compat;
        } else {
            // IAudioClient3 非対応。GetBufferSize 相当の情報は Initialize
            // しないと取れないため、デバイス既定を Compat Lane として報告する。
            caps.defaultPeriodFrames = 0;
            caps.recommendedLane = Lane::Compat;
        }
        CoTaskMemFree(mixFormat);
    }

    client->Release();
    device->Release();
    enumerator->Release();
    return caps;
}

bool WasapiDevice::Open(const DeviceStreamConfig& config, std::wstring* errorOut) {
    auto fail = [&](const wchar_t* msg) {
        if (errorOut) *errorOut = msg;
        Close();
        return false;
    };
    // 共有モードのミックスフォーマット(サンプルレート・チャンネル数)は
    // 決め打ちしない。config.preferredBufferFrames だけ低遅延パスの
    // 周期要求に使う(下記参照)。

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator_))))
        return fail(L"CoCreateInstance(MMDeviceEnumerator) failed");

    HRESULT hr = endpointId_.empty()
        ? enumerator_->GetDefaultAudioEndpoint(isCapture_ ? eCapture : eRender,
                                               eConsole, &device_)
        : enumerator_->GetDevice(endpointId_.c_str(), &device_);
    if (FAILED(hr) || !device_) return fail(L"failed to resolve WASAPI endpoint");

    if (FAILED(device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                 reinterpret_cast<void**>(&audioClient_))))
        return fail(L"IAudioClient Activate failed");

    bool initialized = false;
    if (!config.exclusiveMode) {
        WAVEFORMATEX* mixFormat = nullptr;
        if (FAILED(audioClient_->GetMixFormat(&mixFormat)) || !mixFormat)
            return fail(L"GetMixFormat failed");

        channels_ = mixFormat->nChannels;
        sampleRate_ = mixFormat->nSamplesPerSec;

        // ミックスフォーマットの実体判定。WAVE_FORMAT_EXTENSIBLE の場合、
        // SubFormat GUID は「先頭 4 バイト(Data1)=通常の wFormatTag 値」という
        // 規約で作られているため、専用の GUID 定数を持ち込まなくても判定できる。
        WORD formatTag = mixFormat->wFormatTag;
        if (formatTag == WAVE_FORMAT_EXTENSIBLE) {
            auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mixFormat);
            formatTag = (WORD)ext->SubFormat.Data1;
        }
        if (formatTag == WAVE_FORMAT_IEEE_FLOAT) {
            formatIsFloat_ = true;
            bitsPerSample_ = 32;
        } else if (formatTag == WAVE_FORMAT_PCM) {
            formatIsFloat_ = false;
            bitsPerSample_ = mixFormat->wBitsPerSample;
            if (bitsPerSample_ != 16 && bitsPerSample_ != 24 && bitsPerSample_ != 32) {
                CoTaskMemFree(mixFormat);
                return fail(L"unsupported PCM bit depth (16/24/32 only)");
            }
        } else {
            CoTaskMemFree(mixFormat);
            return fail(L"unsupported WASAPI mix format (PCM/IEEE_FLOAT only)");
        }

        // 低遅延パス(実装ガイド §5.2.1〜§5.2.3): config.aggressiveLowLatency が
        // true のときだけ IAudioClient3 の小バッファ要求パスを試す。既定
        // (false)ではデバイス既定周期(defaultPeriod)で開く ——
        // 小バッファ要求は同じエンドポイントを使う他アプリを巻き込むため、
        // オプトインにする(実装ガイド §5.2.3「誠実さの問題」)。
        IAudioClient3* client3 = nullptr;
        if (SUCCEEDED(audioClient_->QueryInterface(__uuidof(IAudioClient3),
                                                   reinterpret_cast<void**>(&client3)))) {
            UINT32 defaultPeriod = 0, fundamentalPeriod = 0, minPeriod = 0, maxPeriod = 0;
            if (QuerySharedModePeriod(audioClient_, mixFormat, config.rawMode, &defaultPeriod,
                                       &fundamentalPeriod, &minPeriod, &maxPeriod)) {
                UINT32 periodFrames = defaultPeriod;
                if (config.aggressiveLowLatency && config.preferredBufferFrames > 0) {
                    periodFrames = wasapi::ChoosePeriodFrames(
                        (UINT32)config.preferredBufferFrames, minPeriod, maxPeriod, fundamentalPeriod);
                }

                HRESULT hrInit = client3->InitializeSharedAudioStream(
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK, periodFrames, mixFormat, nullptr);

                if (hrInit == AUDCLNT_E_ENGINE_PERIODICITY_LOCKED) {
                    // 実装ガイド §5.2.2: 複数の WASAPI デバイスを同時に開く
                    // 本アプリでは頻繁に起きるため、正常系として扱う。現在の
                    // エンジン周期を問い合わせて合わせる(10ms フォールバックへは
                    // 落とさない)。
                    WAVEFORMATEX* curFmt = nullptr;
                    UINT32 curPeriod = 0;
                    if (SUCCEEDED(client3->GetCurrentSharedModeEnginePeriod(&curFmt, &curPeriod))) {
                        hrInit = client3->InitializeSharedAudioStream(
                            AUDCLNT_STREAMFLAGS_EVENTCALLBACK, curPeriod, curFmt, nullptr);
                        if (curFmt) CoTaskMemFree(curFmt);
                    }
                }
                initialized = SUCCEEDED(hrInit);
            }
            client3->Release();
        }
        if (!initialized) {
            // IAudioClient3 非対応、またはロック後の再初期化も失敗した場合の
            // 最終フォールバック: 10ms 周期を要求(100ns 単位: 1ms = 10,000)。
            constexpr REFERENCE_TIME kBufferDuration = 10 * 10000;
            initialized = SUCCEEDED(audioClient_->Initialize(
                AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                kBufferDuration, 0, mixFormat, nullptr));
        }
        CoTaskMemFree(mixFormat);
        mixFormat = nullptr;
        if (!initialized) return fail(L"IAudioClient Initialize failed");
    } else {
        // 排他モード(実装ガイド §5.2.5)。GetMixFormat は共有エンジン専用の
        // 概念のため使えない。config から候補フォーマットを組み立て、
        // IsFormatSupported(EXCLUSIVE, ...) で確認する
        // (排他モードでは ppClosestMatch は必ず nullptr — 非 null を渡すと
        // E_INVALIDARG になる。共有モードのような「近い形式への丸め」機能は
        // そもそも無い)。
        channels_ = config.channels;
        sampleRate_ = config.sampleRate;

        WAVEFORMATEXTENSIBLE fmt = BuildCandidateFormat(config, /*floatFormat=*/true);
        HRESULT hrFmt = audioClient_->IsFormatSupported(
            AUDCLNT_SHAREMODE_EXCLUSIVE, reinterpret_cast<WAVEFORMATEX*>(&fmt), nullptr);
        if (FAILED(hrFmt)) {
            fmt = BuildCandidateFormat(config, /*floatFormat=*/false);  // 16bit PCM へフォールバック
            hrFmt = audioClient_->IsFormatSupported(
                AUDCLNT_SHAREMODE_EXCLUSIVE, reinterpret_cast<WAVEFORMATEX*>(&fmt), nullptr);
        }
        if (FAILED(hrFmt))
            return fail(L"exclusive mode: no supported format at the requested rate/channels");

        formatIsFloat_ = (fmt.SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT);
        bitsPerSample_ = fmt.Format.wBitsPerSample;

        REFERENCE_TIME defaultPeriod = 0, minPeriod = 0;
        if (FAILED(audioClient_->GetDevicePeriod(&defaultPeriod, &minPeriod)))
            return fail(L"exclusive mode: GetDevicePeriod failed");
        REFERENCE_TIME reqPeriod = defaultPeriod;
        if (config.preferredBufferFrames > 0) {
            const REFERENCE_TIME want = (REFERENCE_TIME)(
                (double)config.preferredBufferFrames * 1.0e7 / sampleRate_ + 0.5);
            reqPeriod = std::max(want, minPeriod);
        }

        HRESULT hrInit = audioClient_->Initialize(
            AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, reqPeriod,
            reqPeriod, reinterpret_cast<WAVEFORMATEX*>(&fmt), nullptr);
        if (hrInit == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            // MSDN の既定手順: 一度アラインされたバッファサイズを取得し、
            // IAudioClient を作り直して同じ長さ(秒換算)で再 Initialize する
            // (排他モード特有の要求。一度 Initialize に失敗した
            // IAudioClient はそのまま再利用できない)。
            UINT32 alignedFrames = 0;
            if (FAILED(audioClient_->GetBufferSize(&alignedFrames)))
                return fail(L"exclusive mode: GetBufferSize after alignment retry failed");
            const REFERENCE_TIME alignedPeriod =
                (REFERENCE_TIME)(1.0e7 * alignedFrames / sampleRate_ + 0.5);
            audioClient_->Release();
            audioClient_ = nullptr;
            if (FAILED(device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                         reinterpret_cast<void**>(&audioClient_))))
                return fail(L"exclusive mode: re-Activate after alignment retry failed");
            hrInit = audioClient_->Initialize(
                AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, alignedPeriod,
                alignedPeriod, reinterpret_cast<WAVEFORMATEX*>(&fmt), nullptr);
        }
        initialized = SUCCEEDED(hrInit);
        if (!initialized) return fail(L"exclusive mode: IAudioClient Initialize failed");
    }

    if (FAILED(audioClient_->GetBufferSize(&periodFrames_)))
        return fail(L"GetBufferSize failed");

    // 実装ガイド §2.3「レーン設計」: 64 サンプル達成をもって RT Lane と判定する。
    lane_ = (periodFrames_ <= 64) ? Lane::RT : Lane::Compat;

    audioEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    stopEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!audioEvent_ || !stopEvent_) return fail(L"CreateEvent failed");
    if (FAILED(audioClient_->SetEventHandle(audioEvent_)))
        return fail(L"SetEventHandle failed");

    if (isCapture_) {
        if (FAILED(audioClient_->GetService(__uuidof(IAudioCaptureClient),
                                            reinterpret_cast<void**>(&captureClient_))))
            return fail(L"GetService(IAudioCaptureClient) failed");
    } else {
        if (FAILED(audioClient_->GetService(__uuidof(IAudioRenderClient),
                                            reinterpret_cast<void**>(&renderClient_))))
            return fail(L"GetService(IAudioRenderClient) failed");
    }

    REFERENCE_TIME latency = 0;
    if (SUCCEEDED(audioClient_->GetStreamLatency(&latency)))
        latencySeconds_ = (double)latency / 1.0e7;  // 100ns 単位 → 秒

    // RT 開始前にリング/スクラッチを全て確保しておく(RT 中は伸びない)
    rings_.clear();
    rings_.reserve((size_t)channels_);
    const size_t cap = RingCapacityFor(periodFrames_);
    for (int c = 0; c < channels_; ++c)
        rings_.push_back(std::make_unique<SpscRing<float>>(cap));
    channelScratch_.assign((size_t)channels_ * periodFrames_, 0.0f);

    // ホットプラグ検知の登録
    auto* nc = new NotificationClient(this, endpointId_);
    notifyClient_ = nc;
    enumerator_->RegisterEndpointNotificationCallback(nc);

    return true;
}

void WasapiDevice::Start() {
    if (!audioClient_ || running_.load()) return;
    if (FAILED(audioClient_->Start())) return;
    running_.store(true);

    // gap 5: まず RTWQ 経路を試す(rtwq_worker.h の設計コメント参照:
    // 対応 OS でない、または DLL ロード/ロック取得/初回キューイングの
    // いずれかに失敗したら、既存の std::thread 経路にフォールバックする)。
    rtwqActive_ = false;
    if (rtwq::Api::Instance().Supported()) {
        DWORD taskId = 0;
        if (SUCCEEDED(rtwq::Api::Instance().LockSharedWorkQueue(L"Pro Audio", &taskId,
                                                                 &rtwqQueueId_)) &&
            SUCCEEDED(rtwq::Api::Instance().CreateAsyncResult(&rtwqCallback_, &rtwqAsyncResult_))) {
            rtwqCallback_.SetQueueId(rtwqQueueId_);
            if (!rtwqIdleEvent_) rtwqIdleEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (rtwqIdleEvent_ &&
                SUCCEEDED(rtwq::Api::Instance().PutWaitingWorkItem(audioEvent_, rtwqAsyncResult_))) {
                rtwqActive_ = true;
            }
        }
        if (!rtwqActive_) {
            // 部分的に確保したリソースを片付けて、下のフォールバックへ進む。
            if (rtwqAsyncResult_) { rtwqAsyncResult_->Release(); rtwqAsyncResult_ = nullptr; }
            if (rtwqQueueId_ != 0) {
                rtwq::Api::Instance().UnlockWorkQueue(rtwqQueueId_);
                rtwqQueueId_ = 0;
            }
        }
    }
    if (!rtwqActive_) {
        thread_ = std::thread(&WasapiDevice::ThreadMain, this);
    }
}

void WasapiDevice::Stop() {
    if (running_.exchange(false)) {
        if (rtwqActive_) {
            // 保留中の RtwqPutWaitingWorkItem を起こして、Invoke 側に
            // running_==false を気付かせる(OBS の WASAPISource::Stop() と
            // 同じ手筋: 待っているのと同じハンドルを SetEvent しないと
            // 起きてくれない)。Invoke が「もう再キューしない」と決めたら
            // rtwqIdleEvent_ を立てるので、それを待ってから片付ける。
            if (audioEvent_) SetEvent(audioEvent_);
            if (rtwqIdleEvent_) WaitForSingleObject(rtwqIdleEvent_, 5000);
            if (rtwqAsyncResult_) { rtwqAsyncResult_->Release(); rtwqAsyncResult_ = nullptr; }
            if (rtwqQueueId_ != 0) {
                rtwq::Api::Instance().UnlockWorkQueue(rtwqQueueId_);
                rtwqQueueId_ = 0;
            }
            rtwqActive_ = false;
        } else {
            if (stopEvent_) SetEvent(stopEvent_);
            if (thread_.joinable()) thread_.join();
        }
    }
    if (audioClient_) audioClient_->Stop();
}

void WasapiDevice::Close() {
    Stop();

    if (notifyClient_) {
        if (enumerator_) enumerator_->UnregisterEndpointNotificationCallback(notifyClient_);
        notifyClient_->Release();
        notifyClient_ = nullptr;
    }
    if (captureClient_) { captureClient_->Release(); captureClient_ = nullptr; }
    if (renderClient_)  { renderClient_->Release();  renderClient_  = nullptr; }
    if (audioClient_)   { audioClient_->Release();   audioClient_   = nullptr; }
    if (device_)        { device_->Release();        device_        = nullptr; }
    if (enumerator_)    { enumerator_->Release();     enumerator_    = nullptr; }
    if (audioEvent_)    { CloseHandle(audioEvent_);   audioEvent_    = nullptr; }
    if (stopEvent_)     { CloseHandle(stopEvent_);    stopEvent_     = nullptr; }
    if (rtwqIdleEvent_) { CloseHandle(rtwqIdleEvent_); rtwqIdleEvent_ = nullptr; }

    rings_.clear();
    channelScratch_.clear();
    channels_ = 0;
    periodFrames_ = 0;
}

// ===========================================================================
// IAudioDevice 契約
// ===========================================================================
SpscRing<float>* WasapiDevice::CaptureRing(int ch) {
    if (!isCapture_ || ch < 0 || ch >= (int)rings_.size()) return nullptr;
    return rings_[(size_t)ch].get();
}
SpscRing<float>* WasapiDevice::RenderRing(int ch) {
    if (isCapture_ || ch < 0 || ch >= (int)rings_.size()) return nullptr;
    return rings_[(size_t)ch].get();
}

DeviceStatus WasapiDevice::Status() const {
    DeviceStatus s;
    s.callbackCount = cbCount_.load(std::memory_order_relaxed);
    s.underrunCount = underrunCount_.load(std::memory_order_relaxed);
    s.overrunCount = overrunCount_.load(std::memory_order_relaxed);
    s.bufferSizeFrames = (long)periodFrames_;
    s.effectiveLatencySeconds = latencySeconds_;
    s.resetRequested = resetRequested_.load(std::memory_order_relaxed);
    s.lane = lane_;
    return s;
}

// ===========================================================================
// RT 側: audioEvent_ が 1 回シグナルされたときの処理本体。
// フォールバック経路(ThreadMain、専用スレッド)と RTWQ 経路
// (RtwqCallback::Invoke、OS 共有ワークキュー上)の両方から呼ばれる、
// 完全に共通のロジック(gap 5)。
// ===========================================================================
void WasapiDevice::OnAudioSignal() {
    cbCount_.fetch_add(1, std::memory_order_relaxed);
    if (isCapture_) {
        ProcessOneCapture();
    } else {
        UINT32 padding = 0;
        if (SUCCEEDED(audioClient_->GetCurrentPadding(&padding))) {
            UINT32 available = periodFrames_ > padding ? periodFrames_ - padding : 0;
            if (available > 0) ProcessOneRender(available);
        }
    }
}

HRESULT STDMETHODCALLTYPE WasapiDevice::RtwqCallback::Invoke(IRtwqAsyncResult* /*result*/) {
    if (owner_->running_.load(std::memory_order_acquire)) {
        owner_->OnAudioSignal();
    }
    // OnAudioSignal() の実行中に Stop() が呼ばれた可能性もあるため、
    // 再アームするかどうかは実行後にもう一度確認する
    // (「止めたはずなのに次のコールバックが来る」事故を防ぐ)。
    if (owner_->running_.load(std::memory_order_acquire)) {
        rtwq::Api::Instance().PutWaitingWorkItem(owner_->audioEvent_, owner_->rtwqAsyncResult_);
    } else if (owner_->rtwqIdleEvent_) {
        SetEvent(owner_->rtwqIdleEvent_);
    }
    return S_OK;
}

// ===========================================================================
// フォールバック経路: RTWQ が使えない(セットアップ失敗・非対応 OS)場合の
// 専用スレッド + イベント駆動ループ(gap 5、rtwq_worker.h 参照)。
// ===========================================================================
void WasapiDevice::ThreadMain() {
    // このスレッド専用の COM 初期化(main スレッドの STA とは別)
    const bool comInit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));

    DWORD taskIndex = 0;
    HANDLE avrtHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    HANDLE waitHandles[2] = { stopEvent_, audioEvent_ };
    while (running_.load(std::memory_order_relaxed)) {
        DWORD wait = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) break;               // stopEvent_
        if (wait != WAIT_OBJECT_0 + 1) continue;         // それ以外は無視して再待機

        OnAudioSignal();
    }

    if (avrtHandle) AvRevertMmThreadCharacteristics(avrtHandle);
    if (comInit) CoUninitialize();
}

void WasapiDevice::ProcessOneCapture() {
    UINT32 totalFrames = 0;
    UINT32 packetLength = 0;
    HRESULT hr = captureClient_->GetNextPacketSize(&packetLength);
    const int bytesPerSample = bitsPerSample_ / 8;
    const int frameBytes = bytesPerSample * channels_;

    while (SUCCEEDED(hr) && packetLength != 0) {
        BYTE* data = nullptr;
        UINT32 numFrames = 0;
        DWORD flags = 0;
        hr = captureClient_->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr);
        if (FAILED(hr)) break;

        const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        for (UINT32 f = 0; f < numFrames; ++f) {
            for (int c = 0; c < channels_; ++c) {
                float v = 0.0f;
                if (!silent) {
                    const uint8_t* p = data + (size_t)f * frameBytes + (size_t)c * bytesPerSample;
                    v = formatIsFloat_ ? *reinterpret_cast<const float*>(p)
                                       : PcmToFloat(p, bytesPerSample);
                }
                channelScratch_[(size_t)c * periodFrames_ + f] = v;
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

void WasapiDevice::ProcessOneRender(UINT32 availableFrames) {
    // エンジンにブロック境界を通知し、RenderRing へ新しいデータを
    // 書き込ませてから(実装ガイド §5.4.1 のマスターコールバック相当)、
    // 実際に WASAPI バッファへ書き出す。
    if (blockCallback_) blockCallback_((int)availableFrames);

    BYTE* data = nullptr;
    if (FAILED(renderClient_->GetBuffer(availableFrames, &data))) return;

    for (int c = 0; c < channels_; ++c) {
        float* chBuf = channelScratch_.data() + (size_t)c * periodFrames_;
        size_t got = rings_[(size_t)c]->Read(chBuf, availableFrames);
        if (got < availableFrames) {
            std::fill(chBuf + got, chBuf + availableFrames, 0.0f);
            underrunCount_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    const int bytesPerSample = bitsPerSample_ / 8;
    const int frameBytes = bytesPerSample * channels_;
    for (UINT32 f = 0; f < availableFrames; ++f) {
        for (int c = 0; c < channels_; ++c) {
            float v = channelScratch_[(size_t)c * periodFrames_ + f];
            uint8_t* p = data + (size_t)f * frameBytes + (size_t)c * bytesPerSample;
            if (formatIsFloat_) {
                std::memcpy(p, &v, sizeof(float));
            } else {
                FloatToPcm(v, p, bytesPerSample);
            }
        }
    }
    renderClient_->ReleaseBuffer(availableFrames, 0);
}

}  // namespace wasapi
