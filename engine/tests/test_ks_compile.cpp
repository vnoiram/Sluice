// test_ks_compile.cpp : KsDevice(DirectKS バックエンド)がコンパイル/リンク
// できることを確認するだけの最小テスト。test_wasapi_compile.cpp と同じ理由
// (Windows コンテナに実 KS 対応オーディオデバイスが無い)で、実際のピン
// 生成・オープンは行わない。
//   - EnumerateKsAudioDevices: 関数ポインタを取得するだけでリンカがシンボル
//     を解決することを確認する(呼び出さない)。
//   - KsDevice: Open()/Start() を呼ばず構築/破棄だけ行う(コンストラクタは
//     KsDeviceInfo を保持するだけ、デストラクタの Close() は何もオープン
//     されていなければ INVALID_HANDLE_VALUE チェックで即 return するため安全)。

#include "device/ks_device.h"

int main() {
    auto enumerateFn = &ks::EnumerateKsAudioDevices;
    (void)enumerateFn;

    ks::KsDevice device(ks::KsDeviceInfo{}, /*isCapture=*/true);
    (void)device;

    return 0;
}
