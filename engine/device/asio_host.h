#pragma once
// asio_host.h : ASIO ドライバのロードと薄いラッパ
//
// ============================================================================
// ★ 初学者向け: ASIO ホストで最初に詰まる 3 つの罠
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
// ============================================================================

#include <windows.h>

#include <array>
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "iasiodrv.h"   // ASIO SDK (thirdparty/asiosdk/common)

namespace asiohost {

constexpr int kMaxInstances = 8;   // 同時に開ける ASIO ドライバ数の上限

struct DriverInfo {
    std::wstring name;   // レジストリのキー名 or Description
    CLSID clsid{};
};

// HKLM\SOFTWARE\ASIO を列挙
std::vector<DriverInfo> EnumerateDrivers();

// デバイスから毎ブロック呼ばれるユーザ処理。
//   index      : ダブルバッファ面 (0/1)
//   このコールバックは各ドライバの RT スレッドから呼ばれる。
using ProcessFn = std::function<void(long index)>;

class AsioDevice {
public:
    AsioDevice() = default;
    ~AsioDevice() { Close(); }
    AsioDevice(const AsioDevice&) = delete;

    // 全工程: CoCreateInstance → init → setSampleRate → createBuffers。
    // useInput=true なら入力 2ch、false なら出力 2ch を開く(PoC 簡略化)。
    bool Open(const DriverInfo& info, double sampleRate, bool useInput,
              std::wstring* errorOut);
    void Start();
    void Stop();
    void Close();

    long BufferSize() const { return bufferSize_; }
    int  SampleTypeCh0() const { return chType_[0]; }

    // RT: bufferSwitch 内から使うヘルパ
    //   入力面 index の 2ch を float インターリーブで dst に変換
    void ConvertInToFloat(long index, float* dstInterleaved) const;
    //   float インターリーブを出力面 index の 2ch に変換
    void ConvertFloatToOut(long index, const float* srcInterleaved) const;

    void SetProcessCallback(ProcessFn fn) { process_ = std::move(fn); }

    // kAsioResetRequest が来たら true になる。監視側が Reopen する
    bool ResetRequested() const { return resetRequested_.load(); }
    void ClearResetRequest() { resetRequested_.store(false); }

    uint64_t CallbackCount() const { return cbCount_.load(); }

private:
    // --- トランポリンから呼ばれる実体 ---
    void OnBufferSwitch(long index);
    void OnSampleRateChanged(double srate);
    long OnAsioMessage(long selector, long value);

    // スロット管理(実装は .cpp)
    static bool  AcquireSlot(AsioDevice* self, int* slotOut);
    static void  ReleaseSlot(int slot);

    IASIO* asio_ = nullptr;
    int slot_ = -1;
    bool isInput_ = false;
    long bufferSize_ = 0;
    ASIOBufferInfo bufInfo_[2]{};   // 2ch 固定
    int chType_[2]{};               // ASIOSampleType
    ProcessFn process_;
    std::atomic<bool> resetRequested_{false};
    std::atomic<uint64_t> cbCount_{0};

    friend struct Trampoline;   // .cpp 内のトランポリンにアクセスを許可
};

} // namespace asiohost
