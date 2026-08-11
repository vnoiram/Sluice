#pragma once
// com_server.h : 単一 CLSID の in-proc COM サーバの最小実装。
//
// vasio.dll は以前、ASIO SDK 付属の common/combase.cpp・common/dllentry.cpp
// (CUnknown/CFactoryTemplate/CClassFactory/DllGetClassObject/DllCanUnloadNow)
// をそのままリンクしていた。これはそれらの独自(SDK 非依存)代替。
//
// Steinberg 版は複数 CLSID を跨いで使える汎用フレームワークだが、vasio は
// 常に単一の CLSID(自分自身)しか登録しないため、もっと薄く書ける:
//   - RefCounted は IUnknown を継承しない、参照カウントの計算だけを提供する
//     ヘルパー。ASIO ドライバクラス(例: SluiceVasioDriver)は
//     `IASIO`(これ自体が IUnknown を継承している)と多重継承する形で使う —
//     もし RefCounted 自体も IUnknown を継承していると、IASIO 経由と
//     RefCounted 経由の 2 系統の IUnknown 基底ができてしまい(ダイヤモンド
//     継承)、AddRef/Release の呼び出しが曖昧になる。IUnknown を名乗るのは
//     常にドライバクラス自身 1 つだけにする。
//   - COM 集約(pUnkOuter による委譲)はサポートしない。ASIO ドライバを
//     集約付きで生成する実在のホスト/DAW事例は無いという前提の割り切り
//     (CoCreateInstance の pUnkOuter に非 null が渡された場合は
//     CLASS_E_NOAGGREGATION を返す)。
//   - QueryInterface は各ドライバクラスの .cpp 側で実装する。ASIO は
//     「標準の IID ではなくドライバ自身の CLSID を IID として比較する」と
//     いう非標準の作法(asio-abi/README.md 参照)を使うため、この関数だけは
//     共通化できない。

// WIN32_LEAN_AND_MEAN 付きで <windows.h> を先にインクルードすると
// <objbase.h> が自動では入らない罠がある(asio_abi.h と同じ理由)。
#include <windows.h>
#include <objbase.h>

#include <atomic>

namespace comserver {

// プロセス全体で生存している COM オブジェクト数(DllCanUnloadNow の判定に
// 使う)。複数の .cpp から参照するため、ヘッダ側で inline variable として
// 定義する(C++17、ODR 安全)。
inline std::atomic<long> g_liveObjectCount{0};

// 参照カウントの計算だけを提供するヘルパー(IUnknown は継承しない。理由は
// 上のコメント参照)。派生クラスは AddRef()/Release() の実装から
// DoAddRef()/DoRelease() を呼ぶだけでよい。
class RefCounted {
public:
    RefCounted() { g_liveObjectCount.fetch_add(1, std::memory_order_relaxed); }
    virtual ~RefCounted() { g_liveObjectCount.fetch_sub(1, std::memory_order_relaxed); }
    RefCounted(const RefCounted&) = delete;
    RefCounted& operator=(const RefCounted&) = delete;

    ULONG DoAddRef() {
        return static_cast<ULONG>(refCount_.fetch_add(1, std::memory_order_relaxed) + 1);
    }
    // 参照が 0 になったら `delete this` する。呼び出し元のクラスが
    // RefCounted を(直接・間接問わず)基底に持っている必要がある
    // (`delete this` は RefCounted* 経由で行うため、~RefCounted() が
    // virtual であることが導出クラスの完全な破棄に必須)。
    ULONG DoRelease() {
        const long remaining = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) delete this;
        return static_cast<ULONG>(remaining);
    }

private:
    std::atomic<long> refCount_{1};  // COM の慣習どおり、生成時点で参照 1 個ぶん
};

// 単一 CLSID 用の IClassFactory 実装。createFn はオブジェクト 1 個を生成して
// IUnknown* を返す関数ポインタ(参照カウント 1 の状態で返すこと。
// CreateInstance 内で QueryInterface 後にこちらの参照は解放する)。
class SingleClassFactory : public IClassFactory {
public:
    using CreateInstanceFn = IUnknown* (*)();

    explicit SingleClassFactory(CreateInstanceFn createFn) : createFn_(createFn) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override;
    STDMETHODIMP LockServer(BOOL fLock) override;

private:
    std::atomic<long> refCount_{1};
    CreateInstanceFn createFn_;
};

// vasio_driver.cpp 側の DllGetClassObject/DllCanUnloadNow から 1 回ずつ
// 呼ぶだけでよいヘルパー。
//   classId  : このサーバが提供する唯一の CLSID。
//   createFn : そのクラスのインスタンスを 1 個生成する関数
//              (例: []{ return static_cast<IUnknown*>(new SluiceVasioDriver()); }
//              に相当するプレーンな関数ポインタ)。
HRESULT DllGetClassObjectImpl(REFCLSID rclsid, const CLSID& classId, REFIID riid, void** ppv,
                              SingleClassFactory::CreateInstanceFn createFn);
HRESULT DllCanUnloadNowImpl();

}  // namespace comserver
