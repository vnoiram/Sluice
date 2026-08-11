#pragma once
// rtwq_worker.h : Real-Time Work Queue(RTWQ)による WASAPI RT スレッドの
// 代替駆動機構(gap 5「WASAPI / KS の RTWQ」)。
//
// 現状の WasapiDevice::ThreadMain は専用の std::thread +
// AvSetMmThreadCharacteristicsW(L"Pro Audio") でイベント駆動ループを回す。
// RTWQ(RtwqLockSharedWorkQueue(L"Pro Audio", ...) + RtwqPutWaitingWorkItem)
// を使うと、OS 共有の MMCSS ワークキュー上でコールバックが呼ばれるため、
// スレッド管理・優先度昇格を OS に委譲できる(実装ガイドが 64 サンプル
// 運用で推奨する経路)。
//
// ただし RTWQ は Windows 8.1 以降でのみ利用可能で、かつ実運用での信頼性に
// 既知の懸念がある: 大手 OSS プロジェクト OBS Studio
// (plugins/win-wasapi/win-wasapi.cpp)には「RTWQ は Win 8.1 で入ったが、
// 原因不明でデスクトップオーディオのキャプチャに時々失敗する」という趣旨の
// コメントがあり、同プロジェクトも常に安全な std::thread 経路への自動
// フォールバック付きでしか使っていない。本実装も同じ方針を踏襲する:
//   - RTWorkQ.dll を LoadLibrary で動的ロードし、無ければ(古い Windows 等)
//     即座に「非対応」扱いにする(実行時ソフト依存。リンク時の必須
//     依存にはしない)。
//   - セットアップ(DLL ロード・ロック取得・初回 PutWaitingWorkItem)が
//     1 箇所でも失敗したら「非対応」として扱い、呼び出し側
//     (WasapiDevice::Start)は既存の std::thread+WaitForMultipleObjects
//     経路にフォールバックする。この経路は実装ガイド既存のコードで
//     既に動作確認済みのため、安全網として機能する。
//   - コールバックオブジェクトはヒープ確保・COM 参照カウントによる
//     自動解放を行わない(OBS の ARtwqAsyncCallback と同じ割り切り:
//     所有者(WasapiDevice)の寿命に組み込みメンバとして紐付ける。
//     AddRef/Release はカウンタを動かすだけで delete しない)。
//   - IRtwqAsyncResult は起動時に 1 回だけ作り、以後の
//     RtwqPutWaitingWorkItem 呼び出しで使い回す(OBS と同じく、RT
//     コールバック 1 回ごとの再確保を避ける)。
//
// 参考にした実装: https://github.com/obsproject/obs-studio
//   plugins/win-wasapi/win-wasapi.cpp (ARtwqAsyncCallback, WASAPISource)

#include <windows.h>
#include <rtworkq.h>

#include <atomic>

namespace rtwq {

// RTWorkQ.dll を動的ロードし、必要な関数ポインタを解決するシングルトン。
// プロセス内で一度ロードしたら保持したままでよい(他の WasapiDevice
// インスタンスも同じハンドルを使い回すため、自前で参照カウントして
// FreeLibrary するより「プロセス終了まで保持」の方が単純で安全)。
class Api {
public:
    static Api& Instance() {
        static Api instance;
        return instance;
    }

    bool Supported() const { return supported_; }

    HRESULT LockSharedWorkQueue(const wchar_t* usageClass, DWORD* taskId, DWORD* id) const {
        return lockSharedWorkQueue_(usageClass, 0, taskId, id);
    }
    HRESULT UnlockWorkQueue(DWORD id) const { return unlockWorkQueue_(id); }
    HRESULT CreateAsyncResult(IRtwqAsyncCallback* callback, IRtwqAsyncResult** result) const {
        return createAsyncResult_(nullptr, callback, nullptr, result);
    }
    HRESULT PutWaitingWorkItem(HANDLE hEvent, IRtwqAsyncResult* result) const {
        return putWaitingWorkItem_(hEvent, 0, result, nullptr);
    }

private:
    Api() {
        module_ = LoadLibraryW(L"RTWorkQ.dll");
        if (!module_) return;

        auto lock = reinterpret_cast<decltype(&RtwqLockSharedWorkQueue)>(
            GetProcAddress(module_, "RtwqLockSharedWorkQueue"));
        auto unlock = reinterpret_cast<decltype(&RtwqUnlockWorkQueue)>(
            GetProcAddress(module_, "RtwqUnlockWorkQueue"));
        auto create = reinterpret_cast<decltype(&RtwqCreateAsyncResult)>(
            GetProcAddress(module_, "RtwqCreateAsyncResult"));
        auto putWaiting = reinterpret_cast<decltype(&RtwqPutWaitingWorkItem)>(
            GetProcAddress(module_, "RtwqPutWaitingWorkItem"));
        if (!lock || !unlock || !create || !putWaiting) return;

        lockSharedWorkQueue_ = lock;
        unlockWorkQueue_ = unlock;
        createAsyncResult_ = create;
        putWaitingWorkItem_ = putWaiting;
        supported_ = true;
    }

    HMODULE module_ = nullptr;
    bool supported_ = false;

    decltype(&RtwqLockSharedWorkQueue) lockSharedWorkQueue_ = nullptr;
    decltype(&RtwqUnlockWorkQueue) unlockWorkQueue_ = nullptr;
    decltype(&RtwqCreateAsyncResult) createAsyncResult_ = nullptr;
    decltype(&RtwqPutWaitingWorkItem) putWaitingWorkItem_ = nullptr;
};

// OBS の ARtwqAsyncCallback と同じ設計判断(上記コメント参照): ヒープ
// 確保・COM 参照カウントによる自動解放を行わない。所有者クラスの
// 組み込みメンバとして使うこと。
class AsyncCallback : public IRtwqAsyncCallback {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IRtwqAsyncCallback) || riid == __uuidof(IUnknown)) {
            *ppv = static_cast<IRtwqAsyncCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)++refCount_; }
    ULONG STDMETHODCALLTYPE Release() override { return (ULONG)--refCount_; }

    HRESULT STDMETHODCALLTYPE GetParameters(DWORD* pdwFlags, DWORD* pdwQueue) override {
        *pdwFlags = 0;
        *pdwQueue = queueId_;
        return S_OK;
    }

    // 罠: RTWorkQ.h の IRtwqAsyncCallback::Invoke/GetParameters は(MIDL 生成の
    // 多くの COM インターフェースと違い)PURE 指定が無い普通の virtual
    // 宣言(本体無し)。このクラス(中間基底)が Invoke を override しない
    // ままだと、このクラス自身の vtable のスロットが未定義シンボル
    // 「IRtwqAsyncCallback::Invoke」を指そうとしてリンクエラーになる
    // (実機ビルドで確認済み)。実際の処理は派生クラス(WasapiDevice::
    // RtwqCallback 等)が必ず override するので、ここでは vtable の穴を
    // 埋めるためだけのダミー実装(呼ばれない想定)。
    HRESULT STDMETHODCALLTYPE Invoke(IRtwqAsyncResult* /*result*/) override { return E_NOTIMPL; }

    void SetQueueId(DWORD id) { queueId_ = id; }
    DWORD GetQueueId() const { return queueId_; }

protected:
    std::atomic<long> refCount_{1};
    DWORD queueId_ = 0;
};

}  // namespace rtwq
