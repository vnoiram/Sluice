// test_com_server.cpp : com_server.h/.cpp のオフライン回帰テスト。
//
// COM 型(REFIID/HRESULT 等)に依存するため Windows でのみビルド・実行
// できる(vasio/tests/test_shared_protocol.cpp と違い、Windows Docker
// 経由の検証が必要 — asio-abi/tests/CMakeLists.txt 参照)。実際に
// CoCreateInstance/DLL ロード/DAW とのやり取りはせず、RefCounted の
// 参照カウントと SingleClassFactory/DllGetClassObjectImpl/
// DllCanUnloadNowImpl の分岐ロジックだけを検証する。

#include "../com_server.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

void Fail(const std::string& what) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    std::exit(1);
}

// --- RefCounted のテスト用最小派生クラス ---------------------------------
class TestRefCounted : public comserver::RefCounted {
public:
    TestRefCounted() = default;
};

void TestRefCountedBasics() {
    const long before = comserver::g_liveObjectCount.load(std::memory_order_relaxed);

    auto* obj = new TestRefCounted();
    if (comserver::g_liveObjectCount.load(std::memory_order_relaxed) != before + 1) {
        Fail("RefCounted: construction did not increment g_liveObjectCount");
    }

    // 生成直後は参照カウント1(RefCounted のコメント参照)。AddRef で2、
    // 最初の Release で1へ戻る(まだ delete されない)。
    const ULONG afterAddRef = obj->DoAddRef();
    if (afterAddRef != 2) Fail("RefCounted: DoAddRef expected 2, got " + std::to_string(afterAddRef));

    const ULONG afterFirstRelease = obj->DoRelease();
    if (afterFirstRelease != 1) {
        Fail("RefCounted: first DoRelease expected 1, got " + std::to_string(afterFirstRelease));
    }
    if (comserver::g_liveObjectCount.load(std::memory_order_relaxed) != before + 1) {
        Fail("RefCounted: premature destruction after non-zero release");
    }

    // 最後の Release で 0 になり、~RefCounted() が g_liveObjectCount を戻すはず。
    const ULONG afterFinalRelease = obj->DoRelease();
    if (afterFinalRelease != 0) {
        Fail("RefCounted: final DoRelease expected 0, got " + std::to_string(afterFinalRelease));
    }
    if (comserver::g_liveObjectCount.load(std::memory_order_relaxed) != before) {
        Fail("RefCounted: destruction did not decrement g_liveObjectCount back");
    }

    std::printf("PASS: RefCounted basic ref-count and live-object tracking\n");
}

// --- SingleClassFactory::CreateInstance が生成する最小 IUnknown 実装 -----
class TestUnknown : public IUnknown {
public:
    static IUnknown* Create() { return static_cast<IUnknown*>(new TestUnknown()); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown) {
            *ppv = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(refCount_.fetch_add(1, std::memory_order_relaxed) + 1);
    }
    STDMETHODIMP_(ULONG) Release() override {
        const long remaining = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) delete this;
        return static_cast<ULONG>(remaining);
    }

private:
    TestUnknown() = default;
    ~TestUnknown() override = default;

    std::atomic<long> refCount_{1};  // COM の慣習どおり、生成時点で参照 1 個ぶん
};

void TestCreateInstanceRejectsAggregation() {
    comserver::SingleClassFactory factory(&TestUnknown::Create);

    IUnknown* fakeOuter = TestUnknown::Create();  // 非 null であれば中身は何でもよい
    void* ppv = nullptr;
    const HRESULT hr = factory.CreateInstance(fakeOuter, IID_IUnknown, &ppv);
    if (hr != CLASS_E_NOAGGREGATION) {
        Fail("CreateInstance: expected CLASS_E_NOAGGREGATION for non-null pUnkOuter");
    }
    if (ppv != nullptr) Fail("CreateInstance: ppv should be null on aggregation rejection");
    fakeOuter->Release();

    std::printf("PASS: SingleClassFactory::CreateInstance rejects aggregation\n");
}

void TestCreateInstanceSucceeds() {
    comserver::SingleClassFactory factory(&TestUnknown::Create);

    void* ppv = nullptr;
    const HRESULT hr = factory.CreateInstance(nullptr, IID_IUnknown, &ppv);
    if (hr != S_OK) Fail("CreateInstance: expected S_OK for valid creation");
    if (ppv == nullptr) Fail("CreateInstance: expected non-null ppv on success");

    // QueryInterface 成功時に内部で AddRef され、生成直後の自己参照は
    // CreateInstance 側で Release 済み(com_server.cpp 参照)なので、
    // 呼び出し元はちょうど参照 1 個ぶんを持っている。ここで Release すれば
    // 0 になり破棄されるはず。
    auto* created = static_cast<IUnknown*>(ppv);
    const ULONG remaining = created->Release();
    if (remaining != 0) {
        Fail("CreateInstance: expected exactly one outstanding reference, remaining=" +
             std::to_string(remaining));
    }

    std::printf("PASS: SingleClassFactory::CreateInstance succeeds and refcounts correctly\n");
}

void TestDllGetClassObjectImpl() {
    const CLSID knownClsid = {
        0x11111111, 0x2222, 0x3333, {0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb}};
    const CLSID otherClsid = {
        0x99999999, 0x8888, 0x7777, {0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00, 0xff}};

    void* ppv = nullptr;
    HRESULT hr = comserver::DllGetClassObjectImpl(otherClsid, knownClsid, IID_IClassFactory, &ppv,
                                                   &TestUnknown::Create);
    if (hr != CLASS_E_CLASSNOTAVAILABLE) {
        Fail("DllGetClassObjectImpl: expected CLASS_E_CLASSNOTAVAILABLE for CLSID mismatch");
    }
    if (ppv != nullptr) Fail("DllGetClassObjectImpl: ppv should be null on CLSID mismatch");

    ppv = nullptr;
    hr = comserver::DllGetClassObjectImpl(knownClsid, knownClsid, IID_IClassFactory, &ppv,
                                           &TestUnknown::Create);
    if (hr != S_OK) Fail("DllGetClassObjectImpl: expected S_OK for matching CLSID");
    if (ppv == nullptr) Fail("DllGetClassObjectImpl: expected non-null factory pointer");

    auto* factory = static_cast<IClassFactory*>(ppv);
    factory->Release();

    std::printf("PASS: DllGetClassObjectImpl CLSID matching\n");
}

void TestDllCanUnloadNow() {
    // このテストは他のテストの後に、生きているオブジェクトが無い状態
    // (main() での実行順、下記参照)で呼ぶ前提。
    if (comserver::g_liveObjectCount.load(std::memory_order_relaxed) != 0) {
        Fail("DllCanUnloadNowImpl: precondition failed, objects from earlier tests still alive");
    }
    if (comserver::DllCanUnloadNowImpl() != S_OK) {
        Fail("DllCanUnloadNowImpl: expected S_OK when no live objects/locks");
    }

    // SingleClassFactory はスタック上に直接構築し、AddRef/Release(COM の
    // 参照カウント規約)は一切呼ばない — 通常のスコープ終了で破棄させる
    // (Release() を呼んで参照 0 に到達させると `delete this` が走り、
    // スタックオブジェクトに対しては未定義動作になるため)。
    comserver::SingleClassFactory factory(&TestUnknown::Create);
    factory.LockServer(TRUE);
    if (comserver::DllCanUnloadNowImpl() != S_FALSE) {
        Fail("DllCanUnloadNowImpl: expected S_FALSE while locked");
    }

    factory.LockServer(FALSE);
    if (comserver::DllCanUnloadNowImpl() != S_OK) {
        Fail("DllCanUnloadNowImpl: expected S_OK after unlocking");
    }

    std::printf("PASS: DllCanUnloadNowImpl reflects lock state\n");
}

}  // namespace

int main() {
    TestRefCountedBasics();
    TestCreateInstanceRejectsAggregation();
    TestCreateInstanceSucceeds();
    TestDllGetClassObjectImpl();
    TestDllCanUnloadNow();
    std::printf("ALL PASS: asio-abi com_server\n");
    return 0;
}
