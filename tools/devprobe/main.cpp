// main.cpp : tools/devprobe 本体(実装ガイド §2.5「デバイス能力プローブ CLI」)
//
// WASAPI/DirectKS 各デバイスに対して IAudioDevice::Probe() を呼び、
// DeviceCaps(64 サンプル対応可否・推奨レーン・周期情報)を一覧表示する
// だけの軽量診断ツール。実際に Open()/Start() はしない。
//
// engine/main.cpp の --list と表示内容は重なるが、devprobe はフルミキサー
// (と ASIO SDK)を必要としない単体ツールとして独立させている。ASIO は
// tools/latencybench と同じ理由(非再配布の ASIO SDK への依存を避ける)で
// 対象外 — ASIO の能力確認は `sluice-engine.exe --list` を使う。

#include <windows.h>

#include <cstdio>

#include "device/ks_device.h"
#include "device/vac.h"
#include "device/vb_cable.h"
#include "device/wasapi_device.h"

namespace {

constexpr double kSampleRate = 48000.0;

void PrintCaps(const DeviceCaps& caps) {
    wprintf(L"supports64=%s  lane=%s  min=%u  fundamental=%u  default=%u\n",
            caps.supports64 ? L"yes" : L"no", caps.recommendedLane == Lane::RT ? L"RT" : L"Compat",
            caps.minPeriodFrames, caps.fundamentalFrames, caps.defaultPeriodFrames);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    (void)argc;
    (void)argv;

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        fwprintf(stderr, L"CoInitializeEx failed\n");
        return 1;
    }

    wprintf(L"=== WASAPI capture endpoints ===\n");
    for (const auto& ep : wasapi::EnumerateEndpoints(/*isCapture=*/true)) {
        wasapi::WasapiDevice dev(ep.id, /*isCapture=*/true);
        wprintf(L"[%s]  ", ep.name.c_str());
        PrintCaps(dev.Probe(kSampleRate));
    }

    wprintf(L"\n=== WASAPI render endpoints ===\n");
    for (const auto& ep : wasapi::EnumerateEndpoints(/*isCapture=*/false)) {
        wasapi::WasapiDevice dev(ep.id, /*isCapture=*/false);
        wprintf(L"[%s]  ", ep.name.c_str());
        PrintCaps(dev.Probe(kSampleRate));
    }

    // KS フィルタは入出力方向がピンを辿らないと分からない(ks_device.h 冒頭
    // コメント参照)。ここでは isCapture=true として問い合わせるが、
    // KsDevice::Probe() はフィルタを開けるかと周期情報だけを見て方向自体は
    // 判定しないため、実際の方向に関わらず結果は概ね参考値になる。
    wprintf(L"\n=== DirectKS devices ===\n");
    for (const auto& info : ks::EnumerateKsAudioDevices()) {
        ks::KsDevice dev(info, /*isCapture=*/true);
        wprintf(L"[%s]  ", info.friendlyName.c_str());
        PrintCaps(dev.Probe(kSampleRate));
    }

    wprintf(L"\n=== Virtual devices ===\n");
    const auto vb = wasapi::DetectVbCable();
    wprintf(L"VB-CABLE: input=%s output=%s\n", vb.virtualInput ? L"found" : L"not found",
            vb.virtualOutput ? L"found" : L"not found");
    const auto vac = wasapi::DetectVac();
    wprintf(L"VAC lines:");
    if (vac.empty()) {
        wprintf(L" none found\n");
    } else {
        for (const auto& line : vac) wprintf(L" %d", line.lineNumber);
        wprintf(L"\n");
    }

    return 0;
}
