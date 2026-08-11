// test_vasio_bridge_compile.cpp : VasioBridgeDevice がコンパイル/リンクできる
// ことを確認するだけの最小テスト。test_ks_compile.cpp と同じ理由(Windows
// コンテナに実 DAW/vasio.dll のロードが無い)で、Open()/PumpSharedMemory() は
// 呼ばない。デフォルト構築/破棄だけなら共有メモリを一切触らないため安全
// (デストラクタの Close() は mappedBase_ が null なら即座に何もしない)。

#include "device/vasio_bridge_device.h"

int main() {
    vasiobridge::VasioBridgeDevice device;
    (void)device;
    return 0;
}
