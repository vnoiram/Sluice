// test_shared_protocol.cpp : shared_protocol.h のオフライン回帰テスト
//
// Windows API に依存しないため、Windows Docker を経由せずどのプラットフォームでも
// ビルド・実行できる(engine/tests/ の「純粋ロジックはオフラインでテストする」方針
// を踏襲)。レイアウト計算とリング読み書きの正しさのみを検証する。

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../shared_protocol.h"

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "FAIL: %s (line %d)\n", #cond, __LINE__); \
            std::exit(1);                                                 \
        }                                                                 \
    } while (0)

static void TestLayoutSizes() {
    const auto layout = vasio::ComputeLayout(8192);
    CHECK(layout.controlBlockBytes == sizeof(vasio::SharedControlBlock));
    CHECK(layout.ringHeaderBytes == sizeof(vasio::ChannelRingHeader) * 2 * vasio::kMaxChannels);
    CHECK(layout.ringDataBytesPerChannel == sizeof(float) * 8192);
    CHECK(layout.ringDataBytesTotal == layout.ringDataBytesPerChannel * 2 * vasio::kMaxChannels);
    CHECK(layout.totalBytes ==
          layout.controlBlockBytes + layout.ringHeaderBytes + layout.ringDataBytesTotal);

    // オフセットが重ならず、単調に増加すること。
    CHECK(layout.RingHeaderOffset() == layout.controlBlockBytes);
    CHECK(layout.RingDataOffset() == layout.controlBlockBytes + layout.ringHeaderBytes);
    for (int ch = 0; ch < 2 * vasio::kMaxChannels; ++ch) {
        const size_t off = layout.RingDataOffsetForChannel(ch);
        CHECK(off == layout.RingDataOffset() +
                         static_cast<size_t>(ch) * layout.ringDataBytesPerChannel);
    }
}

static void TestRingRoundTrip() {
    constexpr uint32_t kCap = 16;
    vasio::ChannelRingHeader hdr;
    std::vector<float> data(kCap, 0.0f);

    float src[5] = {1, 2, 3, 4, 5};
    uint32_t written = vasio::RingWrite(hdr, data.data(), kCap, src, 5);
    CHECK(written == 5);

    float dst[5] = {};
    uint32_t read = vasio::RingRead(hdr, data.data(), kCap, dst, 5);
    CHECK(read == 5);
    for (int i = 0; i < 5; ++i) CHECK(dst[i] == src[i]);
}

static void TestRingWrapAround() {
    // capacity より小さいリングに何度も書いて読んで、mod インデックスの折り返しを検証する。
    constexpr uint32_t kCap = 4;
    vasio::ChannelRingHeader hdr;
    std::vector<float> data(kCap, 0.0f);

    for (int iter = 0; iter < 10; ++iter) {
        float src[3] = {(float)iter, (float)iter + 1, (float)iter + 2};
        uint32_t written = vasio::RingWrite(hdr, data.data(), kCap, src, 3);
        CHECK(written == 3);
        float dst[3] = {};
        uint32_t read = vasio::RingRead(hdr, data.data(), kCap, dst, 3);
        CHECK(read == 3);
        for (int i = 0; i < 3; ++i) CHECK(dst[i] == src[i]);
    }
}

static void TestRingBackpressure() {
    // 読み出さずに容量を超えて書こうとすると、書き込めた分だけが反映されること。
    constexpr uint32_t kCap = 4;
    vasio::ChannelRingHeader hdr;
    std::vector<float> data(kCap, 0.0f);

    float src[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    uint32_t written = vasio::RingWrite(hdr, data.data(), kCap, src, 8);
    CHECK(written == kCap);  // 満杯までしか書けない

    float dst[8] = {};
    uint32_t read = vasio::RingRead(hdr, data.data(), kCap, dst, 8);
    CHECK(read == kCap);  // 実際に書かれた分しか読めない
}

int main() {
    TestLayoutSizes();
    TestRingRoundTrip();
    TestRingWrapAround();
    TestRingBackpressure();
    std::printf("test_shared_protocol: OK\n");
    return 0;
}
