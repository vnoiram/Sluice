// rt_alloc_guard.cpp : rt/rt_alloc_guard.h のグローバル operator new/delete
// オーバーライド本体。SLUICE_RT_ALLOC_GUARD が定義されているビルドでのみ
// 中身が存在する(定義されていなければ空の翻訳単位になり、グローバル
// operator new/delete は標準ライブラリ既定のままになる)。

#if defined(SLUICE_RT_ALLOC_GUARD)

#include "rt/rt_alloc_guard.h"

#include <cstdlib>
#include <new>

namespace rtguard {
thread_local bool g_rtRegion = false;
std::atomic<long long> g_violations{0};
}  // namespace rtguard

void* operator new(std::size_t sz) {
    if (rtguard::g_rtRegion) rtguard::g_violations.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(sz);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

#endif  // SLUICE_RT_ALLOC_GUARD
