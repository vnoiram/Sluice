// test_format_caps.cpp : format_caps.h のオフライン回帰テスト
// (実装ガイド §2.5)。Windows API に依存しないため、Windows Docker を
// 経由せずどのプラットフォームでもビルド・実行できる。

#include "format_caps.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

void Fail(const std::string& what) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    std::exit(1);
}

void ExpectContains(const std::wstring& haystack, const std::wstring& needle,
                     const std::string& label) {
    if (haystack.find(needle) == std::wstring::npos) {
        Fail(label + ": expected output to contain the given substring");
    }
}

void TestFormatCapsFields() {
    DeviceCaps caps;
    caps.minPeriodFrames = 64;
    caps.fundamentalFrames = 32;
    caps.defaultPeriodFrames = 256;
    caps.supports64 = true;
    caps.recommendedLane = Lane::RT;

    const std::wstring out = FormatCaps(caps);
    ExpectContains(out, L"supports64=yes", "format_caps: supports64=true");
    ExpectContains(out, L"lane=RT", "format_caps: lane=RT");
    ExpectContains(out, L"min=64", "format_caps: minPeriodFrames");
    ExpectContains(out, L"fundamental=32", "format_caps: fundamentalFrames");
    ExpectContains(out, L"default=256", "format_caps: defaultPeriodFrames");

    std::printf("PASS: format_caps fields (RT/supports64)\n");
}

void TestFormatCapsCompatLaneAndFalse() {
    DeviceCaps caps;
    caps.minPeriodFrames = 128;
    caps.fundamentalFrames = 0;
    caps.defaultPeriodFrames = 480;
    caps.supports64 = false;
    caps.recommendedLane = Lane::Compat;

    const std::wstring out = FormatCaps(caps);
    ExpectContains(out, L"supports64=no", "format_caps: supports64=false");
    ExpectContains(out, L"lane=Compat", "format_caps: lane=Compat");
    ExpectContains(out, L"fundamental=0", "format_caps: fundamentalFrames=0");

    std::printf("PASS: format_caps fields (Compat/supports64=false)\n");
}

}  // namespace

int main() {
    TestFormatCapsFields();
    TestFormatCapsCompatLaneAndFalse();
    std::printf("ALL PASS: devprobe format_caps\n");
    return 0;
}
