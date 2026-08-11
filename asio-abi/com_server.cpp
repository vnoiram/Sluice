// com_server.cpp : com_server.h の実装

#include "com_server.h"

namespace comserver {

inline std::atomic<long> g_lockCount{0};

STDMETHODIMP SingleClassFactory::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IClassFactory) {
        *ppv = static_cast<IClassFactory*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) SingleClassFactory::AddRef() {
    return static_cast<ULONG>(refCount_.fetch_add(1, std::memory_order_relaxed) + 1);
}

STDMETHODIMP_(ULONG) SingleClassFactory::Release() {
    const long remaining = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining == 0) delete this;
    return static_cast<ULONG>(remaining);
}

STDMETHODIMP SingleClassFactory::CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    // 集約(pUnkOuter による IUnknown 委譲)は非対応(ヘッダのコメント参照)。
    if (pUnkOuter) return CLASS_E_NOAGGREGATION;

    IUnknown* obj = createFn_();
    if (!obj) return E_OUTOFMEMORY;

    // obj は参照カウント 1(生成直後の自己参照)で返ってくる想定。
    // QueryInterface が成功すればさらに AddRef されるので、ここで
    // 生成直後ぶんの参照を手放す(成功時は呼び出し元が持つ参照だけが残る、
    // 失敗時はここで delete まで完了する)。
    const HRESULT hr = obj->QueryInterface(riid, ppv);
    obj->Release();
    return hr;
}

STDMETHODIMP SingleClassFactory::LockServer(BOOL fLock) {
    if (fLock)
        g_lockCount.fetch_add(1, std::memory_order_relaxed);
    else
        g_lockCount.fetch_sub(1, std::memory_order_relaxed);
    return S_OK;
}

HRESULT DllGetClassObjectImpl(REFCLSID rclsid, const CLSID& classId, REFIID riid, void** ppv,
                              SingleClassFactory::CreateInstanceFn createFn) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (rclsid != classId) return CLASS_E_CLASSNOTAVAILABLE;

    auto* factory = new SingleClassFactory(createFn);
    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

HRESULT DllCanUnloadNowImpl() {
    return (g_liveObjectCount.load(std::memory_order_relaxed) == 0 &&
            g_lockCount.load(std::memory_order_relaxed) == 0)
               ? S_OK
               : S_FALSE;
}

}  // namespace comserver
