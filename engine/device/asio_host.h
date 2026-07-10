#pragma once
// asio_host.h : ASIO ドライバのロードと薄いラッパ(IAudioDevice 実装)
//
// ============================================================================
// ★ 初学者向け: ASIO ホストで最初に詰まる 4 つの罠
// ============================================================================
//
// 罠 1: 「コールバックにユーザーポインタが無い」
//   ASIOCallbacks は素の C 関数ポインタ 4 本で、this や userData を渡す口が
//   存在しない。つまり普通に書くとドライバ 1 個(グローバル 1 個)しか
//   扱えない。本プロジェクトは複数 ASIO 同時が目的なので、
//   「スロット方式のトランポリン」で解決する:
//     - コンパイル時に MakeCallbacks<0>, MakeCallbacks<1>, ... と
//       スロット番号ごとの static 関数セットを生成しておく
//     - AsioDevice 生成時に空きスロットを 1 つ占有し、
//       g_slotOwner[slot] に自分を登録する
//     - static 関数は g_slotOwner[slot] 経由でメンバへ転送する
//
// 罠 2: 「IASIO は正規の COM ではない」
//   メソッドが STDMETHODCALLTYPE(__stdcall)ではなく __thiscall のため、
//   C++ 以外からは直接呼べない。また CoCreateInstance には
//   ドライバの CLSID を rclsid と riid の両方に渡すのが ASIO の流儀。
//
// 罠 3: 「init/start/stop はドライバごとに気難しい」
//   init にはウィンドウハンドルを渡す(GetDesktopWindow() で動くものが
//   多いが、専用の隠しウィンドウが要るドライバもある)。
//   kAsioResetRequest を受けたら RT 外で作り直す必要がある。
//
// 罠 4: 「WIN32_LEAN_AND_MEAN と ASIO SDK の相性」
//   <windows.h> を WIN32_LEAN_AND_MEAN 付きでインクルードすると
//   <objbase.h>(COM の基礎: IUnknown, CoCreateInstance, CLSIDFromString
//   等)が自動では入らない。ASIO SDK の iasiodrv.h は IASIO : public
//   IUnknown を素朴に前提にしているため、objbase.h を明示的に先に
//   インクルードしておかないと「IASIO redefinition」等の意味不明な
//   構文エラーの連鎖になる(実際に踏んだ)。
// ============================================================================

#include <windows.h>
#include <objbase.h>   // 罠4: IUnknown / CoCreateInstance / CLSIDFromString 等

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "device/iaudio_device.h"
#include "iasiodrv.h"   // ASIO SDK (thirdparty/asiosdk/common)
#include "rt/spsc_ring.h"

namespace asiohost {

constexpr int kMaxInstances = 8;   // 同時に開ける ASIO ドライバ数の上限

struct DriverInfo {
    std::wstring name;   // レジストリのキー名 or Description
    CLSID clsid{};
};

// HKLM\SOFTWARE\ASIO を列挙
std::vector<DriverInfo> EnumerateDrivers();

// IAudioDevice 実装。1 インスタンス = 1 方向(入力 or 出力)の ASIO ドライバ。
// 実装ガイド §5.1 の想定どおり、Phase 0 の成果(コールバック・トランポリン、
// サンプル型変換)をそのまま使い、リング入出力だけを IAudioDevice 契約に
// 合わせて内製化している(以前は呼び出し側にコールバックで委譲していた)。
class AsioDevice : public IAudioDevice {
public:
    // info/isInput はこのインスタンスが一生使うドライバの識別情報。
    // 実際のオープンは Open() で行う(IAudioDevice 契約のシグネチャに
    // ドライバ選択の余地が無いため、コンストラクタで先に固定する)。
    AsioDevice(DriverInfo info, bool isInput);
    ~AsioDevice() override { Close(); }
    AsioDevice(const AsioDevice&) = delete;

    // 全工程: CoCreateInstance → init → setSampleRate → createBuffers。
    // config.channels 分の入力 or 出力チャンネルを開く。
    bool Open(const DeviceStreamConfig& config, std::wstring* errorOut) override;
    void Start() override;
    void Stop() override;
    void Close() override;

    SpscRing<float>* CaptureRing(int ch) override;
    SpscRing<float>* RenderRing(int ch) override;
    DeviceStatus Status() const override;
    void SetBlockCallback(std::function<void(int frames)> fn) override {
        blockCallback_ = std::move(fn);
    }

    long BufferSize() const { return bufferSize_; }
    int  Channels() const { return channels_; }

    // kAsioResetRequest 等で立った要求をクリアする(Close→Open で
    // 作り直した後、呼び出し側が呼ぶ)。
    void ClearResetRequest() { resetRequested_.store(false); }

private:
    // --- トランポリンから呼ばれる実体 ---
    void OnBufferSwitch(long index);
    void OnSampleRateChanged(double srate);
    long OnAsioMessage(long selector, long value);

    // サンプル型変換(1ch 分, プレーナ ASIO バッファ ⇔ プレーナ float)
    void ConvertChannelToFloat(int c, long index, float* dst) const;
    void ConvertFloatToChannel(int c, long index, const float* src) const;

    // スロット管理(実装は .cpp)
    static bool  AcquireSlot(AsioDevice* self, int* slotOut);
    static void  ReleaseSlot(int slot);

    DriverInfo info_;
    IASIO* asio_ = nullptr;
    int slot_ = -1;
    bool isInput_ = false;
    int channels_ = 0;
    long bufferSize_ = 0;
    double sampleRate_ = 48000.0;
    std::vector<ASIOBufferInfo> bufInfo_;   // channels_ 個
    std::vector<int> chType_;               // channels_ 個(ASIOSampleType)
    std::vector<std::unique_ptr<SpscRing<float>>> rings_;  // channels_ 個、プレーナ
    std::vector<float> scratch_;            // OnBufferSwitch 内の変換用(起動前確保)
    double latencySeconds_ = 0.0;

    std::function<void(int frames)> blockCallback_;
    std::atomic<bool> resetRequested_{false};
    std::atomic<uint64_t> cbCount_{0};
    std::atomic<uint64_t> underrunCount_{0};
    std::atomic<uint64_t> overrunCount_{0};

    friend struct Trampoline;   // .cpp 内のトランポリンにアクセスを許可
};

} // namespace asiohost
