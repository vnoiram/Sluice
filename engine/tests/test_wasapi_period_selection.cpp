// test_wasapi_period_selection.cpp : ChoosePeriodFrames のオフライン回帰テスト
// (実装ガイド §5.2.1)。Windows API に依存しないため WIN32 ガード不要。

#include <cstdio>
#include <cstdlib>

#include "device/wasapi_period.h"

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "FAIL: %s (line %d)\n", #cond, __LINE__); \
            std::exit(1);                                                  \
        }                                                                  \
    } while (0)

int main() {
    using wasapi::ChoosePeriodFrames;

    // fundamental=4 で 64 が要求どおり通るケース。
    CHECK(ChoosePeriodFrames(64, 32, 4096, 4) == 64);

    // fundamental=441(44.1kHz 系)では 64 は非合法。切り上げで次の合法値へ
    // (441 の倍数のうち 64 以上で最小のもの = 441)。
    CHECK(ChoosePeriodFrames(64, 32, 4096, 441) == 441);

    // 丸め方向は「切り上げ」であること(最近傍丸めではない)。
    // fundamental=10, want=61 → 61 に最も近い倍数は 60 だが、切り上げなら 70。
    CHECK(ChoosePeriodFrames(61, 32, 4096, 10) == 70);

    // want が min を下回る場合は min にクランプしてから丸める。
    CHECK(ChoosePeriodFrames(10, 32, 4096, 16) == 32);

    // want が max を上回る場合は max にクランプする(丸めで max を
    // 超えないよう再クランプも行う)。
    CHECK(ChoosePeriodFrames(5000, 32, 4096, 4) == 4096);

    // fundamental=0(制約なし)はクランプのみ。
    CHECK(ChoosePeriodFrames(64, 32, 4096, 0) == 64);
    CHECK(ChoosePeriodFrames(10, 32, 4096, 0) == 32);

    std::printf("test_wasapi_period_selection: OK\n");
    return 0;
}
