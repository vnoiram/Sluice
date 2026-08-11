// vasio_bridge_device.cpp : device/vasio_bridge_device.h の実装

#include "device/vasio_bridge_device.h"

#include <new>

namespace vasiobridge {

DeviceCaps VasioBridgeDevice::Probe(double /*sampleRate*/) {
    DeviceCaps caps;
    caps.recommendedLane = Lane::RT;
    caps.supports64 = true;
    caps.minPeriodFrames = caps.defaultPeriodFrames = 64;
    caps.fundamentalFrames = 0;  // マスタークロックのブロックサイズにそのまま追従するため制約なし
    return caps;
}

bool VasioBridgeDevice::ConnectSharedMemory(std::wstring* errorOut) {
    layout_ = vasio::ComputeLayout(vasio::kDefaultRingCapacityFrames);

    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                  static_cast<DWORD>(layout_.totalBytes), vasio::MappingName());
    if (!mapping_) {
        if (errorOut) *errorOut = L"CreateFileMapping(vasio shared memory) failed";
        return false;
    }
    // CreateFileMappingW は成功時でも「既存のマッピングを見つけて開いただけ」の
    // 場合に GetLastError() == ERROR_ALREADY_EXISTS を返す(vasio.dll が先に
    // 接続していた場合、実装ガイド §8.1 手順3)。この判定は CreateFileMappingW
    // 直後でなければならない(次の Win32 呼び出しで上書きされるため)。
    const bool alreadyExisted = (GetLastError() == ERROR_ALREADY_EXISTS);

    mappedBase_ = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, layout_.totalBytes);
    if (!mappedBase_) {
        if (errorOut) *errorOut = L"MapViewOfFile(vasio shared memory) failed";
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return false;
    }

    readyEvent_ = CreateEventW(nullptr, /*bManualReset=*/FALSE, /*bInitialState=*/FALSE,
                               vasio::ReadyEventName());
    if (!readyEvent_) {
        if (errorOut) *errorOut = L"CreateEvent(vasio ready event) failed";
        UnmapViewOfFile(mappedBase_);
        mappedBase_ = nullptr;
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return false;
    }

    auto* control = reinterpret_cast<vasio::SharedControlBlock*>(mappedBase_);
    if (!alreadyExisted) {
        // このプロセス(engine)が先に接続した場合: プレースメント new で
        // ゼロクリアされたページ上に POD/atomic を正しく構築する
        // (vasio_driver.cpp の ConnectSharedMemory と対称の処理。どちらが
        // 先でも同じ初期化になる)。
        new (control) vasio::SharedControlBlock();
        auto* ringHeaders = reinterpret_cast<vasio::ChannelRingHeader*>(
            static_cast<uint8_t*>(mappedBase_) + layout_.RingHeaderOffset());
        for (int i = 0; i < 2 * vasio::kMaxChannels; ++i)
            new (&ringHeaders[i]) vasio::ChannelRingHeader();
        control->ringCapacityFrames = vasio::kDefaultRingCapacityFrames;
    }

    return true;
}

bool VasioBridgeDevice::Open(const DeviceStreamConfig& config, std::wstring* errorOut) {
    sampleRate_ = config.sampleRate;
    channels_ = vasio::kMaxChannels;  // vasio.dll 側も既定 8in/8out 固定

    if (!ConnectSharedMemory(errorOut)) return false;

    auto* control = reinterpret_cast<vasio::SharedControlBlock*>(mappedBase_);
    control->sampleRate = sampleRate_;
    control->toEngineChannels = static_cast<uint32_t>(channels_);
    control->fromEngineChannels = static_cast<uint32_t>(channels_);
    control->connectionState.store(static_cast<uint32_t>(vasio::ConnectionState::Connected),
                                   std::memory_order_release);

    captureRings_.reserve(static_cast<size_t>(channels_));
    renderRings_.reserve(static_cast<size_t>(channels_));
    for (int c = 0; c < channels_; ++c) {
        captureRings_.push_back(
            std::make_unique<SpscRing<float>>(vasio::kDefaultRingCapacityFrames));
        renderRings_.push_back(
            std::make_unique<SpscRing<float>>(vasio::kDefaultRingCapacityFrames));
    }
    scratch_.assign(vasio::kDefaultRingCapacityFrames, 0.0f);

    return true;
}

void VasioBridgeDevice::Start() {
    // 専用の RT スレッドを持たない(クラス冒頭コメント参照)。何もしない。
}

void VasioBridgeDevice::Stop() {
    // 同上。PumpSharedMemory はマスターの blockCallback から明示的に呼ばれる
    // だけなので、ここで止めるべき独自スレッドが無い。
}

void VasioBridgeDevice::Close() {
    if (mappedBase_) {
        auto* control = reinterpret_cast<vasio::SharedControlBlock*>(mappedBase_);
        // 実装ガイド §8.1 手順5: 切断を明示する。vasio.dll 側はこれを見て
        // 無音を返し続ける(DAW を待たせない)。
        control->connectionState.store(static_cast<uint32_t>(vasio::ConnectionState::Disconnected),
                                       std::memory_order_release);
    }
    if (readyEvent_) {
        CloseHandle(readyEvent_);
        readyEvent_ = nullptr;
    }
    if (mappedBase_) {
        UnmapViewOfFile(mappedBase_);
        mappedBase_ = nullptr;
    }
    if (mapping_) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
    captureRings_.clear();
    renderRings_.clear();
    scratch_.clear();
}

SpscRing<float>* VasioBridgeDevice::CaptureRing(int ch) {
    if (ch < 0 || static_cast<size_t>(ch) >= captureRings_.size()) return nullptr;
    return captureRings_[static_cast<size_t>(ch)].get();
}

SpscRing<float>* VasioBridgeDevice::RenderRing(int ch) {
    if (ch < 0 || static_cast<size_t>(ch) >= renderRings_.size()) return nullptr;
    return renderRings_[static_cast<size_t>(ch)].get();
}

DeviceStatus VasioBridgeDevice::Status() const {
    DeviceStatus s;
    s.callbackCount = pumpCount_.load(std::memory_order_relaxed);
    s.lane = Lane::RT;
    s.resetRequested = false;
    if (mappedBase_) {
        const auto* control = reinterpret_cast<const vasio::SharedControlBlock*>(mappedBase_);
        s.bufferSizeFrames = static_cast<long>(control->bufferSizeFrames);
        // vasio.dll のワーカースレッドが準備完了イベントを待ちきれず
        // タイムアウトした回数(=engine が SetEvent を送れていない/DAW が
        // 追いつけていない目安)を overrun 相当として表示する。1対1の対応が
        // 無い指標のため、UI 表示以外の制御には使わない。
        s.overrunCount = control->engineTimeoutCount.load(std::memory_order_relaxed);
        s.effectiveLatencySeconds =
            sampleRate_ > 0.0 ? static_cast<double>(s.bufferSizeFrames) / sampleRate_ : 0.0;
    }
    return s;
}

void VasioBridgeDevice::RequestDawReset() {
    if (!mappedBase_) return;
    auto* control = reinterpret_cast<vasio::SharedControlBlock*>(mappedBase_);
    uint32_t expected = static_cast<uint32_t>(vasio::ConnectionState::Connected);
    // Connected のときだけ ResetPending へ遷移させる(CAS が失敗するのは
    // Disconnected 中、またはまだ前回の ResetPending を vasio.dll が消化して
    // いない場合 —— どちらも何もしないのが正しい)。
    control->connectionState.compare_exchange_strong(
        expected, static_cast<uint32_t>(vasio::ConnectionState::ResetPending),
        std::memory_order_acq_rel, std::memory_order_relaxed);
}

void VasioBridgeDevice::PumpSharedMemory(int frames) {
    if (!mappedBase_ || frames <= 0) return;
    if (static_cast<size_t>(frames) > scratch_.size()) frames = static_cast<int>(scratch_.size());

    auto* control = reinterpret_cast<vasio::SharedControlBlock*>(mappedBase_);
    const uint32_t cap = control->ringCapacityFrames;
    if (cap == 0) return;  // 相手がまだ接続していない(レイアウト未確定)

    auto* ringHeaders = reinterpret_cast<vasio::ChannelRingHeader*>(
        static_cast<uint8_t*>(mappedBase_) + layout_.RingHeaderOffset());
    float* ringData =
        reinterpret_cast<float*>(static_cast<uint8_t*>(mappedBase_) + layout_.RingDataOffset());

    // ToEngine(DAW → エンジン): 共有メモリ → プロセスローカル CaptureRing。
    for (int c = 0; c < channels_; ++c) {
        const uint32_t got = vasio::RingRead(ringHeaders[c], ringData + static_cast<size_t>(c) * cap,
                                             cap, scratch_.data(), static_cast<uint32_t>(frames));
        for (uint32_t f = got; f < static_cast<uint32_t>(frames); ++f) scratch_[f] = 0.0f;
        captureRings_[static_cast<size_t>(c)]->Write(scratch_.data(), static_cast<size_t>(frames));
    }

    // FromEngine(エンジン → DAW): プロセスローカル RenderRing → 共有メモリ。
    for (int c = 0; c < channels_; ++c) {
        const size_t got =
            renderRings_[static_cast<size_t>(c)]->Read(scratch_.data(), static_cast<size_t>(frames));
        for (size_t f = got; f < static_cast<size_t>(frames); ++f) scratch_[f] = 0.0f;
        const int channelIndex = vasio::kMaxChannels + c;  // 後半 kMaxChannels 本が FromEngine
        vasio::RingWrite(ringHeaders[channelIndex],
                         ringData + static_cast<size_t>(channelIndex) * cap, cap, scratch_.data(),
                         static_cast<uint32_t>(frames));
    }

    control->bufferSizeFrames = static_cast<uint32_t>(frames);
    pumpCount_.fetch_add(1, std::memory_order_relaxed);

    // 実装ガイド §8.1 手順4: エンジンのマスタークロックが準備完了イベントを
    // 発火させ、vasio.dll のワーカースレッドを起こす。
    if (readyEvent_) SetEvent(readyEvent_);
}

}  // namespace vasiobridge
