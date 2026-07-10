// test_wasapi_compile.cpp : WasapiDevice / ProcessLoopbackDevice が
// コンパイル/リンクできることを確認するだけの最小テスト。
//
// 実 WASAPI 呼び出し(デバイス列挙・オープン・アクティベーション等)は
// あえて行わない。オーディオサービスが動いていない/オーディオデバイスが
// 無い環境(Windows コンテナ等)でも、CI として安全・決定的に実行できる
// ようにするため。
//   - EnumerateEndpoints: 関数ポインタを取得するだけでリンカがシンボルを
//     解決することを確認する(呼び出さない)。
//   - ProcessLoopbackDevice: Open()/Start() を呼ばず構築/破棄だけ行う
//     (コンストラクタは PID を保持するだけ、デストラクタの Close() は
//     何もオープンされていなければ null チェックで即 return するため安全)。

#include "device/process_loopback_device.h"
#include "device/wasapi_device.h"

int main() {
    auto enumerateFn = &wasapi::EnumerateEndpoints;
    (void)enumerateFn;

    wasapi::ProcessLoopbackDevice device(/*targetPid=*/0, /*includeChildren=*/true);
    (void)device;

    return 0;
}
