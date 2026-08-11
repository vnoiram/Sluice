// test_wasapi_compile.cpp : WasapiDevice / ProcessLoopbackDevice / VB-CABLE
// 検出がコンパイル/リンクできることを確認するだけの最小テスト。
//
// 実 WASAPI 呼び出し(デバイス列挙・オープン・アクティベーション等)は
// あえて行わない。オーディオサービスが動いていない/オーディオデバイスが
// 無い環境(Windows コンテナ等)でも、CI として安全・決定的に実行できる
// ようにするため。
//   - EnumerateEndpoints/DetectVbCable: 関数ポインタを取得するだけで
//     リンカがシンボルを解決することを確認する(呼び出さない)。
//   - ProcessLoopbackDevice: Open()/Start() を呼ばず構築/破棄だけ行う
//     (コンストラクタは PID を保持するだけ、デストラクタの Close() は
//     何もオープンされていなければ null チェックで即 return するため安全)。

#include "device/process_loopback_device.h"
#include "device/vac.h"
#include "device/vb_cable.h"
#include "device/wasapi_device.h"

int main() {
    auto enumerateFn = &wasapi::EnumerateEndpoints;
    (void)enumerateFn;
    auto detectVbCableFn = &wasapi::DetectVbCable;
    (void)detectVbCableFn;
    auto detectVacFn = &wasapi::DetectVac;
    (void)detectVacFn;

    wasapi::ProcessLoopbackDevice device(/*targetPid=*/0, /*includeChildren=*/true);
    (void)device;

    return 0;
}
