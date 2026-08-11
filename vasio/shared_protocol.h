#pragma once
// shared_protocol.h : vasio.dll <-> engine プロセス間の共有メモリプロトコル
//                     (実装ガイド §8.1 手順3「共有メモリプロトコル」)
//
// vasio.dll は DAW から見れば「ただの ASIO ドライバ」だが、実体は engine プロセスへの
// 橋渡しでしかない。音声データそのものは CreateFileMapping による共有メモリ上の
// リングバッファで受け渡す(制御はここ、実際のオープン/マップ処理は vasio_driver.cpp
// および将来の engine 側コンシューマがそれぞれ行う)。
//
// 命名規約:
//   マッピングオブジェクト名: "Local\\SluiceVasio.<instanceId>"
//   準備完了イベント名(エンジン→vasio): "Local\\SluiceVasioReady.<instanceId>"
//   instanceId はドライバインスタンスごとに一意な文字列。既定運用では固定 "0" のみ
//   (複数 vasio インスタンス対応は将来課題、実装ガイド §8.1 は既定 8in/8out 単一
//   インスタンスを前提としている)。
//
// 同期(実装ガイド §8.1 手順4):
//   エンジン側のマスタークロックが SetEvent する準備完了イベントを vasio 側が
//   タイムアウト付きで待機し、発火するたびに bufferSwitch を実行する。
//   「仮想 ASIO のクロックはエンジンに従属する」ため、この経路には ASRC が不要
//   (両者とも同一クロックで駆動される)。
//
// 方向の命名(vb_cable.h と同種の混乱を避けるため、"入力/出力" ではなく
// "ToEngine"/"FromEngine" で表現する):
//   ToEngine   : DAW が ASIO 出力として書き込んだ音声。エンジンはこれを
//                「仮想入力」として受け取る(DAW → エンジン)。
//   FromEngine : エンジンが計算した音声。DAW は ASIO 入力としてこれを読む
//                (エンジン → DAW)。
//
// 罠: この構造体一式はプロセス間で共有される POD である。ポインタ型メンバは
// 絶対に持たせてはならない(仮想アドレス空間がプロセスごとに異なるため無意味な
// 値になる)。チャンネルリング領域は SharedControlBlock 直後からのバイトオフセット
// で参照する(ComputeLayout 参照)。
//
// 罠: std::atomic<T> をこのように共有メモリ上の POD に埋め込むのは、両プロセスが
// 同一コンパイラ/ABI(本プロジェクトは vasio.dll と engine を同一トリプレットで
// ビルドする)であり、かつ対象の T がロックフリーである場合にのみ安全。
// x64 の uint32_t/uint64_t は通常ロックフリーだが、念のため IsAlwaysLockFree
// を静的アサートで確認する。

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace vasio {

constexpr uint32_t kProtocolVersion = 1;
constexpr int kMaxChannels = 8;  // 既定 8in/8out(実装ガイド §8.1 手順1)

enum class ConnectionState : uint32_t {
    Disconnected = 0,  // エンジンプロセス未接続、または切断中。vasio は無音を返し続ける
                        // (実装ガイド §8.1 手順5「切断耐性」)
    Connected = 1,
    ResetPending = 2,  // エンジン側でレート/バッファサイズが変わった。vasio は次回
                        // bufferSwitch の前に kAsioResetRequest を DAW に送出し、
                        // 送出後は Connected に自分で戻す(実装ガイド §8.1 手順6)。
};

// 1 チャンネル ぶんのリング管理ヘッダ。データ本体(float の配列)はこのヘッダとは
// 別領域(SharedMemoryLayout 参照)に連続配置される。
struct ChannelRingHeader {
    std::atomic<uint32_t> writePos{0};
    std::atomic<uint32_t> readPos{0};
};

struct SharedControlBlock {
    uint32_t protocolVersion = kProtocolVersion;
    std::atomic<uint32_t> connectionState{static_cast<uint32_t>(ConnectionState::Disconnected)};

    double   sampleRate = 48000.0;
    uint32_t bufferSizeFrames = 0;    // エンジン側が確定したブロックサイズ
    uint32_t ringCapacityFrames = 0;  // 各チャンネルリングの容量(フレーム数)
    uint32_t toEngineChannels = 0;    // DAW → エンジン方向の有効チャンネル数 (<= kMaxChannels)
    uint32_t fromEngineChannels = 0;  // エンジン → DAW 方向の有効チャンネル数 (<= kMaxChannels)

    // 統計(実装ガイド §5.6「UI に必ず出すもの」に将来つなぐための土台。
    // vasio.dll 自身は表示を持たないので、engine 側が IPC 経由で読み出す想定)。
    std::atomic<uint64_t> bufferSwitchCount{0};
    std::atomic<uint64_t> engineTimeoutCount{0};  // 準備完了イベント待ちがタイムアウトした回数

    static_assert(std::atomic<uint32_t>::is_always_lock_free,
                  "共有メモリ越しに使うので lock-free でなければならない");
    static_assert(std::atomic<uint64_t>::is_always_lock_free,
                  "共有メモリ越しに使うので lock-free でなければならない");
};

// 共有メモリ全体のバイトレイアウト。
//   [0, controlBlockBytes)                                : SharedControlBlock
//   [controlBlockBytes, controlBlockBytes+ringHeaderBytes) : ChannelRingHeader × 2*kMaxChannels
//     (前半 kMaxChannels 本 = ToEngine、後半 kMaxChannels 本 = FromEngine。
//      実際に使うのは toEngineChannels/fromEngineChannels 本のみで、残りは未使用領域)
//   [..., totalBytes)                                      : float リングデータ × 2*kMaxChannels 本
struct SharedMemoryLayout {
    size_t controlBlockBytes = 0;
    size_t ringHeaderBytes = 0;
    size_t ringDataBytesPerChannel = 0;
    size_t ringDataBytesTotal = 0;
    size_t totalBytes = 0;

    size_t RingHeaderOffset() const { return controlBlockBytes; }
    size_t RingDataOffset() const { return controlBlockBytes + ringHeaderBytes; }
    // channelIndex: 0..kMaxChannels-1 が ToEngine、kMaxChannels..2*kMaxChannels-1 が FromEngine。
    size_t RingDataOffsetForChannel(int channelIndex) const {
        return RingDataOffset() + static_cast<size_t>(channelIndex) * ringDataBytesPerChannel;
    }
};

constexpr SharedMemoryLayout ComputeLayout(uint32_t ringCapacityFrames) {
    SharedMemoryLayout layout;
    layout.controlBlockBytes = sizeof(SharedControlBlock);
    layout.ringHeaderBytes = sizeof(ChannelRingHeader) * (2 * kMaxChannels);
    layout.ringDataBytesPerChannel = sizeof(float) * static_cast<size_t>(ringCapacityFrames);
    layout.ringDataBytesTotal = layout.ringDataBytesPerChannel * (2 * kMaxChannels);
    layout.totalBytes = layout.controlBlockBytes + layout.ringHeaderBytes + layout.ringDataBytesTotal;
    return layout;
}

inline const wchar_t* MappingName() { return L"Local\\SluiceVasio.0"; }
inline const wchar_t* ReadyEventName() { return L"Local\\SluiceVasioReady.0"; }

// ---------------------------------------------------------------------------
// SPSC リング読み書き(1 チャンネルぶん)
// ---------------------------------------------------------------------------
// engine/rt/spsc_ring.h の SpscRing<float> と同じ「単調増加する write/read
// カウンタを capacityFrames で mod indexing する」方式。ToEngine 側は
// DAW→vasio が書き手・エンジンが読み手、FromEngine 側はその逆であり、各方向は
// 単一生産者・単一消費者(SPSC)であることを呼び出し側が守る必要がある。
// capacityFrames が 2^32 未満のカウンタで割り切れない場合でも、差分
// (writePos - readPos) が常に capacityFrames 以下である限り符号なし演算の
// wraparound は正しく動作する(24h ソークテスト等での長時間運用でも安全)。

inline uint32_t RingWrite(ChannelRingHeader& hdr, float* ringBase, uint32_t capacityFrames,
                           const float* src, uint32_t n) {
    const uint32_t w = hdr.writePos.load(std::memory_order_relaxed);
    const uint32_t r = hdr.readPos.load(std::memory_order_acquire);
    const uint32_t used = w - r;
    uint32_t canWrite = capacityFrames > used ? capacityFrames - used : 0;
    if (n > canWrite) n = canWrite;
    for (uint32_t i = 0; i < n; ++i) ringBase[(w + i) % capacityFrames] = src[i];
    hdr.writePos.store(w + n, std::memory_order_release);
    return n;
}

inline uint32_t RingRead(ChannelRingHeader& hdr, const float* ringBase, uint32_t capacityFrames,
                          float* dst, uint32_t n) {
    const uint32_t r = hdr.readPos.load(std::memory_order_relaxed);
    const uint32_t w = hdr.writePos.load(std::memory_order_acquire);
    uint32_t available = w - r;
    if (n > available) n = available;
    for (uint32_t i = 0; i < n; ++i) dst[i] = ringBase[(r + i) % capacityFrames];
    hdr.readPos.store(r + n, std::memory_order_release);
    return n;
}

}  // namespace vasio
