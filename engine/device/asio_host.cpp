// asio_host.cpp : ASIO ドライバのロード実装
#include "asio_host.h"

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
// Open / Start / Stop / Close
// ===========================================================================
bool AsioDevice::Open(const DriverInfo& info, double sampleRate,
                      bool useInput, std::wstring* errorOut) {
    auto fail = [&](const wchar_t* msg) {
        if (errorOut) *errorOut = msg;
        Close();
        return false;
    };

    isInput_ = useInput;
    if (!AcquireSlot(this, &slot_)) return fail(L"no free callback slot");

    // ASIO の流儀: rclsid と riid の両方にドライバ CLSID を渡す
    HRESULT hr = CoCreateInstance(info.clsid, nullptr, CLSCTX_INPROC_SERVER,
                                  info.clsid,
                                  reinterpret_cast<void**>(&asio_));
    if (FAILED(hr) || !asio_) return fail(L"CoCreateInstance failed");

    if (asio_->init(GetDesktopWindow()) != ASIOTrue) {
        char em[128] = {};
        asio_->getErrorMessage(em);
        // (em は ANSI。ログには変換して出す)
        return fail(L"driver init() failed");
    }

    long nIn = 0, nOut = 0;
    if (asio_->getChannels(&nIn, &nOut) != ASE_OK)
        return fail(L"getChannels failed");
    if (useInput ? nIn < 2 : nOut < 2)
        return fail(L"device does not have 2 channels for this direction");

    if (asio_->canSampleRate(sampleRate) != ASE_OK ||
        asio_->setSampleRate(sampleRate) != ASE_OK)
        return fail(L"sample rate not supported");

    long mn, mx, granularity;
    if (asio_->getBufferSize(&mn, &mx, &bufferSize_, &granularity) != ASE_OK)
        return fail(L"getBufferSize failed");

    // 先頭 2ch を確保
    for (int c = 0; c < 2; ++c) {
        bufInfo_[c].isInput = useInput ? ASIOTrue : ASIOFalse;
        bufInfo_[c].channelNum = c;
        bufInfo_[c].buffers[0] = bufInfo_[c].buffers[1] = nullptr;
    }
    if (asio_->createBuffers(bufInfo_, 2, bufferSize_,
                             &g_callbackTable[slot_]) != ASE_OK)
        return fail(L"createBuffers failed");

    for (int c = 0; c < 2; ++c) {
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
}

// ===========================================================================
// RT 側
// ===========================================================================
void AsioDevice::OnBufferSwitch(long index) {
    cbCount_.fetch_add(1, std::memory_order_relaxed);
    if (process_) process_(index);   // std::function 呼び出し自体は確保しない
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
// サンプル型変換(2ch 固定, プレーナ ASIO バッファ ⇔ インターリーブ float)
// ===========================================================================
void AsioDevice::ConvertInToFloat(long index, float* dst) const {
    constexpr float k = 1.0f / 2147483648.0f;
    const long n = bufferSize_;
    for (int c = 0; c < 2; ++c) {
        const void* src = bufInfo_[c].buffers[index];
        if (chType_[c] == ASIOSTInt32LSB) {
            const int32_t* s = static_cast<const int32_t*>(src);
            for (long i = 0; i < n; ++i) dst[i * 2 + c] = s[i] * k;
        } else { // ASIOSTFloat32LSB
            const float* s = static_cast<const float*>(src);
            for (long i = 0; i < n; ++i) dst[i * 2 + c] = s[i];
        }
    }
}

void AsioDevice::ConvertFloatToOut(long index, const float* src) const {
    const long n = bufferSize_;
    for (int c = 0; c < 2; ++c) {
        void* dstRaw = bufInfo_[c].buffers[index];
        if (chType_[c] == ASIOSTInt32LSB) {
            int32_t* d = static_cast<int32_t*>(dstRaw);
            for (long i = 0; i < n; ++i) {
                float v = src[i * 2 + c];
                v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
                d[i] = static_cast<int32_t>(v * 2147483647.0f);
            }
        } else {
            float* d = static_cast<float*>(dstRaw);
            for (long i = 0; i < n; ++i) d[i] = src[i * 2 + c];
        }
    }
}

} // namespace asiohost
