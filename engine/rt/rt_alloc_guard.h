#pragma once
// rt_alloc_guard.h : RT スレッド内アロケーション検出フック(実装ガイド §3.2
// 「開発ビルドでは RT スレッド内アロケーション検出フックを有効化する」/
// §9 落とし穴チェックリスト)。
//
// これまでこの仕組みは各 test_*.cpp が個別に(グローバル operator new/delete
// を各テスト実行ファイル内でオーバーライドする形で)実装しており、
// 本番ビルド(sluice-engine.exe)には存在しなかった。本ヘッダはその仕組みを
// 1 箇所に切り出し、SLUICE_RT_ALLOC_GUARD が定義されたビルド構成
// (engine/CMakeLists.txt: Debug 構成でのみ既定 ON)でだけ有効化する。
// Release ビルドでは RtGuard は空の struct になり、オーバーヘッドは 0。
//
// 使い方: 実際に RT コールバックが始まる**その場所**(デバイスの
// OnBufferSwitch/ThreadMain のコールバック呼び出し点)の先頭で
// `rtguard::RtGuard guard;` を 1 回だけ作る。ネストして複数回作らないこと
// (thread_local な bool 1 個で管理しているため、内側のデストラクタが
// 外側の区間をまだ RT 中であるにもかかわらず解除してしまう)。
//
// 現状の適用範囲(このフェーズの意図的な割り切り): main.cpp のマスター
// blockCallback(EngineGraph::Process() と vasio ブリッジの
// PumpSharedMemory() を包む、実際の DSP/ルーティング処理のほぼ全体を
// カバーする)にのみ適用している。asio_host.cpp/wasapi_device.cpp/
// ks_device.cpp/process_loopback_device.cpp 自身の OnBufferSwitch/
// ThreadMain(サンプル型変換・リング書き込み)への適用は、既存の
// 実機検証済みコードへの侵襲を避けるため今回は見送っている(将来課題)。

#include <atomic>
#include <cstddef>

namespace rtguard {

#if defined(SLUICE_RT_ALLOC_GUARD)

extern thread_local bool g_rtRegion;
extern std::atomic<long long> g_violations;

struct RtGuard {
    RtGuard() { g_rtRegion = true; }
    ~RtGuard() { g_rtRegion = false; }
    RtGuard(const RtGuard&) = delete;
    RtGuard& operator=(const RtGuard&) = delete;
};

inline long long Violations() { return g_violations.load(std::memory_order_relaxed); }
inline void ResetViolations() { g_violations.store(0, std::memory_order_relaxed); }

#else

struct RtGuard {};
inline long long Violations() { return 0; }
inline void ResetViolations() {}

#endif

}  // namespace rtguard
