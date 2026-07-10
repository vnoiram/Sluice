// test_wasapi_compile.cpp : WasapiDevice がコンパイル/リンクできることを
// 確認するだけの最小テスト。
//
// 実 WASAPI 呼び出し(デバイス列挙・オープン等)はあえて行わない。
// オーディオサービスが動いていない/オーディオデバイスが無い環境
// (Windows コンテナ等)でも、CI として安全・決定的に実行できるように
// するため。関数ポインタを取得するだけでリンカがシンボルを解決する
// ことは確認できる。

#include "device/wasapi_device.h"

int main() {
    auto enumerateFn = &wasapi::EnumerateEndpoints;
    (void)enumerateFn;
    return 0;
}
