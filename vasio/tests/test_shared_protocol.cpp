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

// gap 11: 複数インスタンス対応。既定引数("0")が導入前の固定名と一致し
// (後方互換)、異なる instanceId は異なる名前になることを確認する。
static void TestInstanceNaming() {
    CHECK(vasio::MappingName() == L"Local\\SluiceVasio.0");
    CHECK(vasio::ReadyEventName() == L"Local\\SluiceVasioReady.0");
    CHECK(vasio::MappingName(L"0") == vasio::MappingName());
    CHECK(vasio::ReadyEventName(L"0") == vasio::ReadyEventName());

    CHECK(vasio::MappingName(L"1") == L"Local\\SluiceVasio.1");
    CHECK(vasio::ReadyEventName(L"1") == L"Local\\SluiceVasioReady.1");
    CHECK(vasio::MappingName(L"1") != vasio::MappingName(L"2"));
}

// SharedControlBlock は std::atomic メンバを持つためコピー/ムーブ不可。
// 各ケースごとに個別のインスタンスを組み立て、既定値から 1 フィールドだけ
// 壊す形にする(共有メモリ上の実際の使われ方 = プレースメント new 後に
// フィールドへ直接書き込む、と同じ組み立て方)。
static void SetValidFields(vasio::SharedControlBlock& control) {
    control.ringCapacityFrames = vasio::kDefaultRingCapacityFrames;
    control.toEngineChannels = vasio::kMaxChannels;
    control.fromEngineChannels = vasio::kMaxChannels;
}

static void TestValidateControlBlock() {
    {
        vasio::SharedControlBlock control;
        SetValidFields(control);
        CHECK(vasio::ValidateControlBlock(control, vasio::kDefaultRingCapacityFrames));
    }
    {
        vasio::SharedControlBlock badVersion;
        SetValidFields(badVersion);
        badVersion.protocolVersion = vasio::kProtocolVersion + 1;
        CHECK(!vasio::ValidateControlBlock(badVersion, vasio::kDefaultRingCapacityFrames));
    }
    {
        vasio::SharedControlBlock badCapacity;
        SetValidFields(badCapacity);
        badCapacity.ringCapacityFrames = vasio::kDefaultRingCapacityFrames + 1;
        CHECK(!vasio::ValidateControlBlock(badCapacity, vasio::kDefaultRingCapacityFrames));
    }
    {
        vasio::SharedControlBlock badToChannels;
        SetValidFields(badToChannels);
        badToChannels.toEngineChannels = vasio::kMaxChannels + 1;
        CHECK(!vasio::ValidateControlBlock(badToChannels, vasio::kDefaultRingCapacityFrames));
    }
    {
        vasio::SharedControlBlock badFromChannels;
        SetValidFields(badFromChannels);
        badFromChannels.fromEngineChannels = vasio::kMaxChannels + 1;
        CHECK(!vasio::ValidateControlBlock(badFromChannels, vasio::kDefaultRingCapacityFrames));
    }
}

// capacityFrames == 0 は不正なレイアウト(相手が未接続、または破損)を示す。
// mod 演算のゼロ除算 UB を起こさず、単に何もしないことを確認する。
static void TestRingZeroCapacityGuard() {
    vasio::ChannelRingHeader hdr;
    float dummy[1] = {0.0f};
    float src[1] = {42.0f};
    float dst[1] = {0.0f};

    uint32_t written = vasio::RingWrite(hdr, dummy, /*capacityFrames=*/0, src, 1);
    CHECK(written == 0);

    uint32_t read = vasio::RingRead(hdr, dummy, /*capacityFrames=*/0, dst, 1);
    CHECK(read == 0);
}

int main() {
    TestLayoutSizes();
    TestRingRoundTrip();
    TestRingWrapAround();
    TestRingBackpressure();
    TestInstanceNaming();
    TestValidateControlBlock();
    TestRingZeroCapacityGuard();
    std::printf("test_shared_protocol: OK\n");
    return 0;
}
