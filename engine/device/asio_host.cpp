// asio_host.cpp : ASIO ドライバのロード実装(IAudioDevice 実装)
#include "device/asio_host.h"

#include <algorithm>
#include <cassert>

namespace asiohost {

// ===========================================================================
// レジストリ列挙
// ===========================================================================
std::vector<DriverInfo> EnumerateDrivers() {
    std::vector<DriverInfo> result;
    HKEY hAsio;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0,
                      KEY_READ, &hAsio) != ERROR_SUCCESS) {
        return result;
    }
    for (DWORD i = 0;; ++i) {
        wchar_t sub[256]; DWORD len = 256;
        if (RegEnumKeyExW(hAsio, i, sub, &len, nullptr, nullptr, nullptr,
                          nullptr) != ERROR_SUCCESS) break;
        HKEY hSub;
        if (RegOpenKeyExW(hAsio, sub, 0, KEY_READ, &hSub) != ERROR_SUCCESS)
            continue;

        DriverInfo info;
        info.name = sub;

        wchar_t clsidStr[64]; DWORD sz = sizeof(clsidStr); DWORD type = 0;
        if (RegQueryValueExW(hSub, L"CLSID", nullptr, &type,
                             reinterpret_cast<BYTE*>(clsidStr), &sz)
                == ERROR_SUCCESS && type == REG_SZ) {
            if (SUCCEEDED(CLSIDFromString(clsidStr, &info.clsid))) {
                wchar_t desc[256]; sz = sizeof(desc);
                if (RegQueryValueExW(hSub, L"Description", nullptr, &type,
                                     reinterpret_cast<BYTE*>(desc), &sz)
                        == ERROR_SUCCESS && type == REG_SZ) {
                    info.name = desc;
                }
                result.push_back(info);
            }
        }
        RegCloseKey(hSub);
    }
    RegCloseKey(hAsio);
    return result;
}

// ===========================================================================
// コールバック・トランポリン(asio_host.h の「罠 1」参照)
// ===========================================================================
static std::array<std::atomic<AsioDevice*>, kMaxInstances> g_slotOwner{};

struct Trampoline {
    template <int Slot>
    static void BufferSwitch(long index, ASIOBool /*directProcess*/) {
        if (AsioDevice* d = g_slotOwner[Slot].load(std::memory_order_acquire))
            d->OnBufferSwitch(index);
    }
    template <int Slot>
    static void SampleRateDidChange(ASIOSampleRate rate) {
        if (AsioDevice* d = g_slotOwner[Slot].load(std::memory_order_acquire))
            d->OnSampleRateChanged(rate);
    }
    template <int Slot>
    static long AsioMessage(long selector, long value, void*, double*) {
        if (AsioDevice* d = g_slotOwner[Slot].load(std::memory_order_acquire))
            return d->OnAsioMessage(selector, value);
        return 0;
    }
    template <int Slot>
    static ASIOTime* BufferSwitchTimeInfo(ASIOTime* t, long index,
                                          ASIOBool direct) {
        BufferSwitch<Slot>(index, direct);
        return t;
    }

    template <int Slot>
    static ASIOCallbacks Make() {
        return ASIOCallbacks{
            &BufferSwitch<Slot>, &SampleRateDidChange<Slot>,
            &AsioMessage<Slot>, &BufferSwitchTimeInfo<Slot>};
    }
};

// スロット番号 → コールバックセット のコンパイル時テーブル
template <int... I>
static std::array<ASIOCallbacks, sizeof...(I)>
MakeCallbackTable(std::integer_sequence<int, I...>) {
    return { Trampoline::Make<I>()... };
}
static std::array<ASIOCallbacks, kMaxInstances> g_callbackTable =
    MakeCallbackTable(std::make_integer_sequence<int, kMaxInstances>{});

bool AsioDevice::AcquireSlot(AsioDevice* self, int* slotOut) {
    for (int s = 0; s < kMaxInstances; ++s) {
        AsioDevice* expected = nullptr;
        if (g_slotOwner[s].compare_exchange_strong(expected, self)) {
            *slotOut = s;
            return true;
        }
    }
    return false;
}
void AsioDevice::ReleaseSlot(int slot) {
    if (slot >= 0) g_slotOwner[slot].store(nullptr);
}

// ===========================================================================
// 構築 / Open / Start / Stop / Close
// ===========================================================================
AsioDevice::AsioDevice(DriverInfo info, bool isInput)
    : info_(std::move(info)), isInput_(isInput) {}

namespace {
// リング容量: デバイスバッファサイズの 16 倍を 2 の冪へ切り上げ
// (実装ガイド §4.2 の目安。プレーナなのでチャンネルごとに同じ容量でよい)。
size_t RingCapacityFor(long bufferSize) {
    size_t frames = (size_t)bufferSize * 16;
    size_t cap = 1;
    while (cap < frames) cap <<= 1;
    return cap;
}
}  // namespace

// ===========================================================================
// 能力プローブ(実装ガイド §5.1・§4.1.4)
// ===========================================================================
DeviceCaps AsioDevice::Probe(double sampleRate) {
    DeviceCaps caps;
    caps.recommendedLane = Lane::RT;  // ASIO は常に RT Lane 候補

    if (asio_) {
        // 二重オープン禁止(実装ガイド §4.1.5)のため新規インスタンスは作らず、
        // 既に確定している bufferSize_ から簡易的に返す。
        caps.minPeriodFrames = caps.defaultPeriodFrames = (uint32_t)bufferSize_;
        caps.supports64 = (bufferSize_ == 64);
        return caps;
    }

    IASIO* probe = nullptr;
    HRESULT hr = CoCreateInstance(info_.clsid, nullptr, CLSCTX_INPROC_SERVER,
                                  info_.clsid, reinterpret_cast<void**>(&probe));
    if (FAILED(hr) || !probe) return caps;

    if (probe->init(GetDesktopWindow()) != ASIOTrue) {
        probe->Release();
        return caps;
    }

    if (probe->canSampleRate(sampleRate) == ASE_OK) probe->setSampleRate(sampleRate);

    long mn = 0, mx = 0, preferred = 0, granularity = 0;
    if (probe->getBufferSize(&mn, &mx, &preferred, &granularity) == ASE_OK) {
        caps.minPeriodFrames = (uint32_t)mn;
        caps.defaultPeriodFrames = (uint32_t)preferred;
        caps.fundamentalFrames = granularity > 0 ? (uint32_t)granularity : 0;
        // 実装ガイド §4.1.4 の 64 サンプル合法性判定:
        // 64 >= min && 64 <= max && (granularity <= 0(2 の冪のみ許容) ||
        // (64-min) % granularity == 0)
        caps.supports64 = (64 >= mn && 64 <= mx) &&
                           (granularity <= 0 || (64 - mn) % granularity == 0);
    }

    probe->Release();
    return caps;
}

bool AsioDevice::Open(const DeviceStreamConfig& config, std::wstring* errorOut) {
    auto fail = [&](const wchar_t* msg) {
        if (errorOut) *errorOut = msg;
        Close();
        return false;
    };

    channels_ = config.channels;
    sampleRate_ = config.sampleRate;
    if (!AcquireSlot(this, &slot_)) return fail(L"no free callback slot");

    // ASIO の流儀: rclsid と riid の両方にドライバ CLSID を渡す
    HRESULT hr = CoCreateInstance(info_.clsid, nullptr, CLSCTX_INPROC_SERVER,
                                  info_.clsid,
                                  reinterpret_cast<void**>(&asio_));
    if (FAILED(hr) || !asio_) return fail(L"CoCreateInstance failed");

    if (asio_->init(GetDesktopWindow()) != ASIOTrue) {
        return fail(L"driver init() failed");
    }

    long nIn = 0, nOut = 0;
    if (asio_->getChannels(&nIn, &nOut) != ASE_OK)
        return fail(L"getChannels failed");
    const long available = isInput_ ? nIn : nOut;
    if (available < channels_)
        return fail(L"device does not have enough channels for this direction");

    if (asio_->canSampleRate(sampleRate_) != ASE_OK ||
        asio_->setSampleRate(sampleRate_) != ASE_OK)
        return fail(L"sample rate not supported");

    long mn, mx, granularity;
    if (asio_->getBufferSize(&mn, &mx, &bufferSize_, &granularity) != ASE_OK)
        return fail(L"getBufferSize failed");

    bufInfo_.assign(channels_, ASIOBufferInfo{});
    for (int c = 0; c < channels_; ++c) {
        bufInfo_[c].isInput = isInput_ ? ASIOTrue : ASIOFalse;
        bufInfo_[c].channelNum = c;
        bufInfo_[c].buffers[0] = bufInfo_[c].buffers[1] = nullptr;
    }
    if (asio_->createBuffers(bufInfo_.data(), channels_, bufferSize_,
                             &g_callbackTable[slot_]) != ASE_OK)
        return fail(L"createBuffers failed");

    chType_.assign(channels_, 0);
    for (int c = 0; c < channels_; ++c) {
        ASIOChannelInfo ci{};
        ci.channel = c;
        ci.isInput = bufInfo_[c].isInput;
        if (asio_->getChannelInfo(&ci) != ASE_OK)
            return fail(L"getChannelInfo failed");
        chType_[c] = ci.type;
        if (ci.type != ASIOSTInt32LSB && ci.type != ASIOSTFloat32LSB)
            return fail(L"unsupported sample type (PoC supports "
                        L"Int32LSB/Float32LSB only)");
    }

    long inLatency = 0, outLatency = 0;
    if (asio_->getLatencies(&inLatency, &outLatency) == ASE_OK) {
        const long lat = isInput_ ? inLatency : outLatency;
        latencySeconds_ = (double)lat / sampleRate_;
    }

    // RT 開始前にリング/スクラッチを全て確保しておく(RT 中は伸びない)
    rings_.clear();
    rings_.reserve(channels_);
    const size_t cap = RingCapacityFor(bufferSize_);
    for (int c = 0; c < channels_; ++c)
        rings_.push_back(std::make_unique<SpscRing<float>>(cap));
    scratch_.assign((size_t)bufferSize_, 0.0f);

    return true;
}

void AsioDevice::Start() { if (asio_) asio_->start(); }
void AsioDevice::Stop()  { if (asio_) asio_->stop(); }

void AsioDevice::Close() {
    if (asio_) {
        asio_->stop();
        asio_->disposeBuffers();
        asio_->Release();
        asio_ = nullptr;
    }
    ReleaseSlot(slot_);
    slot_ = -1;
    bufInfo_.clear();
    chType_.clear();
    rings_.clear();
    scratch_.clear();
}

// ===========================================================================
// IAudioDevice 契約
// ===========================================================================
SpscRing<float>* AsioDevice::CaptureRing(int ch) {
    if (!isInput_ || ch < 0 || ch >= (int)rings_.size()) return nullptr;
    return rings_[(size_t)ch].get();
}

SpscRing<float>* AsioDevice::RenderRing(int ch) {
    if (isInput_ || ch < 0 || ch >= (int)rings_.size()) return nullptr;
    return rings_[(size_t)ch].get();
}

DeviceStatus AsioDevice::Status() const {
    DeviceStatus s;
    s.callbackCount = cbCount_.load(std::memory_order_relaxed);
    s.underrunCount = underrunCount_.load(std::memory_order_relaxed);
    s.overrunCount = overrunCount_.load(std::memory_order_relaxed);
    s.bufferSizeFrames = bufferSize_;
    s.effectiveLatencySeconds = latencySeconds_;
    s.resetRequested = resetRequested_.load(std::memory_order_relaxed);
    return s;
}

// ===========================================================================
// RT 側
// ===========================================================================
void AsioDevice::OnBufferSwitch(long index) {
    cbCount_.fetch_add(1, std::memory_order_relaxed);
    const size_t n = (size_t)bufferSize_;

    if (isInput_) {
        // キャプチャ: ASIO バッファ → float 変換 → 各チャンネルの CaptureRing へ
        for (int c = 0; c < channels_; ++c) {
            ConvertChannelToFloat(c, index, scratch_.data());
            if (rings_[(size_t)c]->Write(scratch_.data(), n) < n)
                overrunCount_.fetch_add(1, std::memory_order_relaxed);
        }
        if (blockCallback_) blockCallback_((int)n);  // 「新しい入力データが来た」通知
    } else {
        // レンダー: まずエンジンにブロック境界を通知し、RenderRing へ
        // 新しいデータを書き込ませる(実装ガイド §5.4.1 のマスター
        // コールバックに相当)。そのあとで RenderRing を読み出して出力する。
        if (blockCallback_) blockCallback_((int)n);
        for (int c = 0; c < channels_; ++c) {
            size_t got = rings_[(size_t)c]->Read(scratch_.data(), n);
            if (got < n) {
                std::fill(scratch_.data() + got, scratch_.data() + n, 0.0f);
                underrunCount_.fetch_add(1, std::memory_order_relaxed);
            }
            ConvertFloatToChannel(c, index, scratch_.data());
        }
    }
}

void AsioDevice::OnSampleRateChanged(double) {
    resetRequested_.store(true);     // PoC ではリセット扱いに倒す
}

long AsioDevice::OnAsioMessage(long selector, long /*value*/) {
    switch (selector) {
    case kAsioSelectorSupported: return 1;
    case kAsioEngineVersion:     return 2;
    case kAsioResetRequest:
        resetRequested_.store(true); // RT 外の監視ループが作り直す
        return 1;
    case kAsioResyncRequest:
    case kAsioLatenciesChanged:  return 1;
    default:                     return 0;
    }
}

// ===========================================================================
// サンプル型変換(1ch 分, プレーナ ASIO バッファ ⇔ プレーナ float)
// ===========================================================================
void AsioDevice::ConvertChannelToFloat(int c, long index, float* dst) const {
    constexpr float k = 1.0f / 2147483648.0f;
    const long n = bufferSize_;
    const void* src = bufInfo_[(size_t)c].buffers[index];
    if (chType_[(size_t)c] == ASIOSTInt32LSB) {
        const int32_t* s = static_cast<const int32_t*>(src);
        for (long i = 0; i < n; ++i) dst[i] = s[i] * k;
    } else { // ASIOSTFloat32LSB
        const float* s = static_cast<const float*>(src);
        for (long i = 0; i < n; ++i) dst[i] = s[i];
    }
}

void AsioDevice::ConvertFloatToChannel(int c, long index, const float* src) const {
    const long n = bufferSize_;
    void* dstRaw = bufInfo_[(size_t)c].buffers[index];
    if (chType_[(size_t)c] == ASIOSTInt32LSB) {
        int32_t* d = static_cast<int32_t*>(dstRaw);
        for (long i = 0; i < n; ++i) {
            float v = src[i];
            v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
            d[i] = static_cast<int32_t>(v * 2147483647.0f);
        }
    } else {
        float* d = static_cast<float*>(dstRaw);
        for (long i = 0; i < n; ++i) d[i] = src[i];
    }
}

} // namespace asiohost
