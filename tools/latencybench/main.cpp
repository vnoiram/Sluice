// main.cpp : tools/latencybench 本体(実装ガイド §7.3)
//
// 仮想デバイス(VAC/VB-CABLE 等)の再生側へ M 系列を出力し、同じケーブルの
// 録音側から取得した信号との相互相関で往復レイテンシを測定する。
// {device, accessMethod, requestedBufferFrames, deviceSetting} を総当たりし、
// 実効レイテンシ(ms)・xrun 回数・CPU% を CSV で出力する。
//
// 検証状況: engine/ 配下の Windows 専用コードと同じ制約で、この Linux/WSL
// 開発環境では実機・実 Windows SDK ヘッダでのコンパイル確認ができていない。
// xcorr.h(信号生成・相互相関)のみ Windows API 非依存でありオフライン
// 検証済み(tests/test_xcorr.cpp)。
//
// 現状のスコープ: accessMethod は WASAPI 共有モードと DirectKS
// (engine/device/ks_device.h)のみ対応。WASAPI 排他モードは engine/ 側の
// WasapiDevice が現状共有モードしか実装していないため未対応(TODO、
// §7.3 の測定軸としては将来追加)。

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "device/ks_device.h"
#include "device/vac.h"
#include "device/vb_cable.h"
#include "device/wasapi_device.h"
#include "xcorr.h"

namespace {

enum class AccessMethod { WasapiShared, DirectKs };

const wchar_t* AccessMethodName(AccessMethod m) {
    switch (m) {
        case AccessMethod::WasapiShared: return L"wasapi_shared";
        case AccessMethod::DirectKs: return L"directks";
    }
    return L"unknown";
}

// 1 つの {device, accessMethod, requestedBufferFrames} 組み合わせの測定結果。
struct MeasurementResult {
    bool ok = false;
    double effectiveLatencyMs = 0.0;
    uint64_t xrunCount = 0;
    double cpuPercent = 0.0;
};

// 実行中の CPU 使用率を GetProcessTimes の前後差分から概算する。
double MeasureCpuPercent(const FILETIME& kernelBefore, const FILETIME& userBefore,
                         const FILETIME& kernelAfter, const FILETIME& userAfter,
                         double wallSeconds) {
    auto ToUint64 = [](const FILETIME& ft) {
        return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };
    const uint64_t kernelDelta = ToUint64(kernelAfter) - ToUint64(kernelBefore);
    const uint64_t userDelta = ToUint64(userAfter) - ToUint64(userBefore);
    // FILETIME は 100ns 単位。
    const double cpuSeconds = (double)(kernelDelta + userDelta) / 1.0e7;
    if (wallSeconds <= 0.0) return 0.0;
    return 100.0 * cpuSeconds / wallSeconds;
}

// M 系列を renderDevice へ再生しつつ captureDevice から録音し、相互相関で
// 往復レイテンシを求める。renderDevice/captureDevice は Open()/Start() 済み
// であること(呼び出し側が管理する)。
MeasurementResult RunOneMeasurement(IAudioDevice& renderDevice, IAudioDevice& captureDevice,
                                    double sampleRate, uint32_t bufferFrames) {
    MeasurementResult result;

    const auto mls = latencybench::GenerateMls(9);  // 511 サンプル
    if (mls.empty()) return result;

    // 録音バッファ: M 系列長 + 想定往復レイテンシの余裕(数百ms ぶん)。
    const size_t recordFrames = mls.size() + (size_t)(sampleRate * 0.5);
    std::vector<float> recorded(recordFrames, 0.0f);

    FILETIME creationTime, exitTime, kernelBefore, userBefore;
    GetProcessTimes(GetCurrentProcess(), &creationTime, &exitTime, &kernelBefore, &userBefore);
    const auto wallStart = std::chrono::steady_clock::now();

    // 再生側リングへ M 系列を書き込む(RenderRing はエンジンが書く側の
    // 契約なので、ここでは測定ツール自身がエンジン役になる)。
    SpscRing<float>* renderRing = renderDevice.RenderRing(0);
    SpscRing<float>* captureRing = captureDevice.CaptureRing(0);
    if (!renderRing || !captureRing) return result;

    renderRing->Write(mls.data(), mls.size());

    // 録音バッファが埋まるまでポーリング(簡易実装。RT 経路ではなく
    // 測定ツールの制御スレッドから行う)。
    size_t got = 0;
    const auto timeout = std::chrono::seconds(3);
    const auto pollStart = std::chrono::steady_clock::now();
    while (got < recordFrames) {
        got += captureRing->Read(recorded.data() + got, recordFrames - got);
        if (got >= recordFrames) break;
        if (std::chrono::steady_clock::now() - pollStart > timeout) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const auto wallEnd = std::chrono::steady_clock::now();
    FILETIME kernelAfter, userAfter;
    GetProcessTimes(GetCurrentProcess(), &creationTime, &exitTime, &kernelAfter, &userAfter);

    if (got < mls.size()) return result;  // 録音が足りなければ測定失敗

    const size_t offset =
        latencybench::FindOffsetByCrossCorrelation(mls, recorded, recordFrames - mls.size());

    result.ok = true;
    result.effectiveLatencyMs = (double)offset / sampleRate * 1000.0;
    result.xrunCount = renderDevice.Status().underrunCount + captureDevice.Status().overrunCount;

    const double wallSeconds = std::chrono::duration<double>(wallEnd - wallStart).count();
    result.cpuPercent =
        MeasureCpuPercent(kernelBefore, userBefore, kernelAfter, userAfter, wallSeconds);

    (void)bufferFrames;
    return result;
}

void PrintUsage() {
    std::wprintf(
        L"Usage: latencybench.exe --list\n"
        L"       latencybench.exe --sweep <render-name-substring> <capture-name-substring>\n"
        L"           [--csv <path>]\n"
        L"\n"
        L"  --list   VB-CABLE/VAC らしき WASAPI デバイスを列挙する\n"
        L"  --sweep  指定した再生/録音デバイス(仮想ケーブルの両端)で\n"
        L"           accessMethod x requestedBufferFrames を総当たりし、\n"
        L"           CSV を出力する\n");
}

void ListVirtualDevices() {
    auto vbCable = wasapi::DetectVbCable();
    if (vbCable.virtualInput)
        std::wprintf(L"VB-CABLE virtual input (capture) : %s\n", vbCable.virtualInput->name.c_str());
    if (vbCable.virtualOutput)
        std::wprintf(L"VB-CABLE virtual output (render) : %s\n",
                     vbCable.virtualOutput->name.c_str());

    for (const auto& line : wasapi::DetectVac()) {
        std::wprintf(L"VAC Line %d:\n", line.lineNumber);
        if (line.capture) std::wprintf(L"  capture: %s\n", line.capture->name.c_str());
        if (line.render) std::wprintf(L"  render : %s\n", line.render->name.c_str());
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    if (wcscmp(argv[1], L"--list") == 0) {
        ListVirtualDevices();
        return 0;
    }

    if (wcscmp(argv[1], L"--sweep") != 0 || argc < 4) {
        PrintUsage();
        return 1;
    }

    const std::wstring renderNameSubstr = argv[2];
    const std::wstring captureNameSubstr = argv[3];
    std::wstring csvPath;
    for (int i = 4; i + 1 < argc; ++i)
        if (wcscmp(argv[i], L"--csv") == 0) csvPath = argv[i + 1];

    // レンダー/キャプチャエンドポイントを名前の部分一致で検索する。
    std::wstring renderId, captureId;
    for (const auto& ep : wasapi::EnumerateEndpoints(/*isCapture=*/false))
        if (ep.name.find(renderNameSubstr) != std::wstring::npos) renderId = ep.id;
    for (const auto& ep : wasapi::EnumerateEndpoints(/*isCapture=*/true))
        if (ep.name.find(captureNameSubstr) != std::wstring::npos) captureId = ep.id;

    if (renderId.empty() || captureId.empty()) {
        std::wprintf(L"device not found (render=\"%s\" capture=\"%s\")\n",
                     renderNameSubstr.c_str(), captureNameSubstr.c_str());
        return 1;
    }

    std::vector<uint32_t> bufferSizes = {64, 128, 256, 512};
    std::vector<AccessMethod> methods = {AccessMethod::WasapiShared, AccessMethod::DirectKs};

    std::vector<std::string> csvLines;
    csvLines.push_back(
        "device,accessMethod,requestedBufferFrames,effectiveLatencyMs,xrunCount,cpuPercent");

    for (AccessMethod method : methods) {
        for (uint32_t bufferFrames : bufferSizes) {
            DeviceStreamConfig config;
            config.channels = 1;
            config.sampleRate = 48000.0;
            config.preferredBufferFrames = (long)bufferFrames;
            config.aggressiveLowLatency = true;

            std::unique_ptr<IAudioDevice> renderDevice, captureDevice;
            if (method == AccessMethod::WasapiShared) {
                renderDevice = std::make_unique<wasapi::WasapiDevice>(renderId, false);
                captureDevice = std::make_unique<wasapi::WasapiDevice>(captureId, true);
            } else {
                // DirectKS は WasapiDevice とは別列挙(SetupDiGetClassDevs)なので、
                // 名前一致で該当フィルタを探す。
                std::vector<ks::KsDeviceInfo> ksDevices = ks::EnumerateKsAudioDevices();
                ks::KsDeviceInfo renderInfo, captureInfo;
                bool foundRender = false, foundCapture = false;
                for (const auto& d : ksDevices) {
                    if (!foundRender && d.friendlyName.find(renderNameSubstr) != std::wstring::npos) {
                        renderInfo = d;
                        foundRender = true;
                    }
                    if (!foundCapture &&
                        d.friendlyName.find(captureNameSubstr) != std::wstring::npos) {
                        captureInfo = d;
                        foundCapture = true;
                    }
                }
                if (!foundRender || !foundCapture) continue;  // KS 経路にこの名前の一致なし
                renderDevice = std::make_unique<ks::KsDevice>(renderInfo, false);
                captureDevice = std::make_unique<ks::KsDevice>(captureInfo, true);
            }

            std::wstring err;
            if (!renderDevice->Open(config, &err) || !captureDevice->Open(config, &err)) {
                std::wprintf(L"open failed (%s, buffer=%u): %s\n", AccessMethodName(method),
                             bufferFrames, err.c_str());
                continue;
            }
            renderDevice->Start();
            captureDevice->Start();

            MeasurementResult r =
                RunOneMeasurement(*renderDevice, *captureDevice, config.sampleRate, bufferFrames);

            renderDevice->Stop();
            captureDevice->Stop();
            renderDevice->Close();
            captureDevice->Close();

            char line[256];
            if (r.ok) {
                std::snprintf(line, sizeof(line), "loopback,%ls,%u,%.3f,%llu,%.2f",
                              AccessMethodName(method), bufferFrames, r.effectiveLatencyMs,
                              (unsigned long long)r.xrunCount, r.cpuPercent);
            } else {
                std::snprintf(line, sizeof(line), "loopback,%ls,%u,FAILED,,", AccessMethodName(method),
                              bufferFrames);
            }
            csvLines.push_back(line);
            std::wprintf(L"%hs\n", line);
        }
    }

    if (!csvPath.empty()) {
        std::wofstream out(csvPath);
        for (const auto& l : csvLines) out << l.c_str() << L"\n";
    }

    return 0;
}
