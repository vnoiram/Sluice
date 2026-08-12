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
// スコープ: accessMethod は WASAPI 共有モード・WASAPI 排他モード
// (gap 6・9、engine/device/wasapi_device.h の DeviceStreamConfig::
// exclusiveMode)・DirectKS(engine/device/ks_device.h)に対応。
//
// gap 9 (VAC 側): --vac-line/--vac-ms-per-int を指定すると
// vac_registry.h 経由でレジストリの "Milliseconds per interrupt" を書き換え
// つつスイープする。公式マニュアルにレジストリレイアウトの一次資料はある
// ものの、実機の VAC インストールでの動作確認ができていないため実験的機能
// 扱い(vac_registry.h のコメント参照)。既定では無効(フラグ未指定なら
// 従来どおり)。
//
// gap 9 (VB-CABLE 側): 内部レイテンシ/サンプルレートは VBCABLE_ControlPanel
// (要管理者権限の GUI)経由でのみ設定され、公式にドキュメント化された
// レジストリキーが見当たらないため、当て推量での自動化はしていない。
// tools/latencybench/README.md に、実機で reg export の差分を取って
// 一次情報化する手順を記載した。
//
// --loopback: VAC/VB-CABLE のような「レンダー/キャプチャの両端を持つ
// ケーブル」構造ではなく、レンダー(仮想スピーカー)しか持たない仮想デバイス
// (例: VirtualDrivers/Virtual-Audio-Driver、MIT ライセンスのオープンソース
// WDM ドライバ。ソースを直接確認したところ、その仮想スピーカーと仮想マイクは
// 内部で繋がっておらず、既存の「render→同じケーブルの capture で拾う」方式
// では測定できない)向けに、WASAPI loopback capture(対象ドライバの対応が
// 不要な OS 標準機能。engine/device/wasapi_device.h の WasapiDevice の
// loopback コンストラクタ引数)でレンダーエンドポイント自身を録音側として
// 使う。AccessMethod::WasapiSharedLoopback 参照。

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
#include "ipc/json_value.h"
#include "vac_registry.h"
#include "xcorr.h"

namespace {

// engine/main.cpp の WideToUtf8 と同じ実装(gap 8: --json 出力で
// ipc::LatencyMeasurement のフィールドが UTF-8 std::string を要求するため)。
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int len =
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), len, nullptr, nullptr);
    return out;
}

// gap 9: WasapiExclusive を追加(WASAPI 排他モード、gap 6 で
// DeviceStreamConfig::exclusiveMode が実装されたことで測定可能になった)。
//
// WasapiSharedLoopback: VAC/VB-CABLE のような「録音側エンドポイントを別途
// 持つケーブル」構造ではなく、VirtualDrivers/Virtual-Audio-Driver のように
// レンダー(仮想スピーカー)しか持たず、録音側の仮想マイクとは内部で
// 繋がっていない仮想デバイス向け。--loopback 指定時のみ使う
// (engine/device/wasapi_device.h の WasapiDevice loopback コンストラクタ
// 引数、WASAPI loopback capture という OS 標準機能を使うため、対象ドライバ
// 側の対応・レジストリ知識は不要)。
enum class AccessMethod { WasapiShared, WasapiExclusive, WasapiSharedLoopback, DirectKs };

const wchar_t* AccessMethodName(AccessMethod m) {
    switch (m) {
        case AccessMethod::WasapiShared: return L"wasapi_shared";
        case AccessMethod::WasapiExclusive: return L"wasapi_exclusive";
        case AccessMethod::WasapiSharedLoopback: return L"wasapi_shared_loopback";
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
        L"           [--csv <path>] [--json <path>]\n"
        L"           [--vac-line <N> --vac-ms-per-int <v1,v2,...>]\n"
        L"           [--loopback]\n"
        L"\n"
        L"  --list   VB-CABLE/VAC らしき WASAPI デバイスを列挙する\n"
        L"  --sweep  指定した再生/録音デバイス(仮想ケーブルの両端)で\n"
        L"           accessMethod x requestedBufferFrames を総当たりし、\n"
        L"           CSV を出力する\n"
        L"  --json   engine/ipc/latency_db.h が読む形式(gap 8: 実測値を\n"
        L"           DeviceCaps.measuredLatencyMs へ自動反映するための入力)\n"
        L"           で測定結果を書き出す。失敗した組み合わせは含めない。\n"
        L"  --vac-line / --vac-ms-per-int\n"
        L"           gap 9(実験的機能、要管理者権限、実機未検証): 指定した\n"
        L"           VAC ケーブル番号(--list の \"VAC Line N\" の N)に対し、\n"
        L"           レジストリの \"Milliseconds per interrupt\" を \n"
        L"           <v1,v2,...>(各 1..20)へ順に書き換えつつ外側でスイープ\n"
        L"           する。レジストリ書き込み・ドライバ再起動・読み戻し確認の\n"
        L"           いずれかに失敗した設定値はスキップされ、CSV/JSON には\n"
        L"           含まれない。詳細・既知の未検証事項は\n"
        L"           tools/latencybench/vac_registry.h と README.md を参照。\n"
        L"  --loopback\n"
        L"           VAC/VB-CABLE のような「録音側エンドポイントを別途持つ\n"
        L"           ケーブル」ではなく、レンダー(仮想スピーカー)しか持たない\n"
        L"           仮想デバイス(例: VirtualDrivers/Virtual-Audio-Driver)向け。\n"
        L"           <render-name-substring> だけを使い、WASAPI loopback\n"
        L"           capture(OS 標準機能)でそのレンダーエンドポイント自身を\n"
        L"           録音側として測定する。<capture-name-substring> は無視\n"
        L"           されるが引数としては必要(renderNameSubstr と同じ値を\n"
        L"           渡せばよい)。accessMethod は wasapi_shared_loopback のみ\n"
        L"           (排他モード・DirectKS は対象外)。\n");
}

void ListVirtualDevices() {
    bool foundHeuristic = false;

    auto vbCable = wasapi::DetectVbCable();
    if (vbCable.virtualInput) {
        std::wprintf(L"VB-CABLE virtual input (capture) : %s\n", vbCable.virtualInput->name.c_str());
        foundHeuristic = true;
    }
    if (vbCable.virtualOutput) {
        std::wprintf(L"VB-CABLE virtual output (render) : %s\n",
                     vbCable.virtualOutput->name.c_str());
        foundHeuristic = true;
    }

    for (const auto& line : wasapi::DetectVac()) {
        std::wprintf(L"VAC Line %d:\n", line.lineNumber);
        if (line.capture) std::wprintf(L"  capture: %s\n", line.capture->name.c_str());
        if (line.render) std::wprintf(L"  render : %s\n", line.render->name.c_str());
        foundHeuristic = true;
    }

    if (!foundHeuristic) {
        std::wprintf(L"(VB-CABLE/VAC らしき名前のデバイスは見つからなかった。"
                    L"VirtualDrivers/Virtual-Audio-Driver のような他の仮想デバイスは"
                    L"名前のパターンが違うため、以下の全デバイス一覧から探すこと。"
                    L"見つけた名前をそのまま --sweep の引数に使えばよい。)\n");
    }

    // VAC/VB-CABLE の名前ヒューリスティックに一致しない仮想デバイス
    // (VirtualDrivers/Virtual-Audio-Driver 等)は上のパターンマッチでは
    // 拾えないため、実際に --sweep へ渡す名前を確認できるよう全デバイスも
    // 併記する。
    std::wprintf(L"\nすべての WASAPI レンダーエンドポイント:\n");
    for (const auto& ep : wasapi::EnumerateEndpoints(/*isCapture=*/false))
        std::wprintf(L"  %s\n", ep.name.c_str());

    std::wprintf(L"\nすべての WASAPI キャプチャエンドポイント:\n");
    for (const auto& ep : wasapi::EnumerateEndpoints(/*isCapture=*/true))
        std::wprintf(L"  %s\n", ep.name.c_str());
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
    std::wstring csvPath, jsonPath;
    int vacLineNumber = -1;
    std::vector<int> vacMsPerIntValues;  // 空なら未使用(gap 9: VAC 内部設定スイープ、実験的機能)
    bool loopbackMode = false;  // --loopback: render エンドポイント自身を WASAPI loopback capture で拾う
    for (int i = 4; i < argc; ++i) {
        if (wcscmp(argv[i], L"--loopback") == 0) loopbackMode = true;
        if (i + 1 < argc) {
            if (wcscmp(argv[i], L"--csv") == 0) csvPath = argv[i + 1];
            if (wcscmp(argv[i], L"--json") == 0) jsonPath = argv[i + 1];
            if (wcscmp(argv[i], L"--vac-line") == 0) vacLineNumber = _wtoi(argv[i + 1]);
            if (wcscmp(argv[i], L"--vac-ms-per-int") == 0) {
                const std::wstring list = argv[i + 1];
                size_t start = 0;
                while (start <= list.size()) {
                    const size_t comma = list.find(L',', start);
                    const std::wstring token = list.substr(
                        start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
                    if (!token.empty()) vacMsPerIntValues.push_back(_wtoi(token.c_str()));
                    if (comma == std::wstring::npos) break;
                    start = comma + 1;
                }
            }
        }
    }

    if (!vacMsPerIntValues.empty() && vacLineNumber < 0) {
        std::wprintf(L"--vac-ms-per-int には --vac-line <N> の指定も必要です\n");
        return 1;
    }

    // レンダー/キャプチャエンドポイントを名前の部分一致で検索する。
    // --loopback 指定時、captureNameSubstr(第2引数)はレンダー側の
    // WASAPI loopback capture で代替するため使わない
    // (CLI の位置引数の形はそのまま保つ。renderNameSubstr と同じ値を渡す
    // 運用を README.md で案内)。
    std::wstring renderId, captureId;
    for (const auto& ep : wasapi::EnumerateEndpoints(/*isCapture=*/false))
        if (ep.name.find(renderNameSubstr) != std::wstring::npos) renderId = ep.id;
    if (!loopbackMode) {
        for (const auto& ep : wasapi::EnumerateEndpoints(/*isCapture=*/true))
            if (ep.name.find(captureNameSubstr) != std::wstring::npos) captureId = ep.id;
    }

    if (renderId.empty() || (!loopbackMode && captureId.empty())) {
        std::wprintf(L"device not found (render=\"%s\" capture=\"%s\")\n",
                     renderNameSubstr.c_str(), captureNameSubstr.c_str());
        return 1;
    }

    std::vector<uint32_t> bufferSizes = {64, 128, 256, 512};
    // --loopback: レンダーエンドポイント自身を WASAPI loopback capture で
    // 拾う(排他モードは仕様上不可、DirectKS はそもそも別軸の話なので、
    // このモードでは wasapi_shared_loopback だけを測定する)。
    std::vector<AccessMethod> methods =
        loopbackMode ? std::vector<AccessMethod>{AccessMethod::WasapiSharedLoopback}
                     : std::vector<AccessMethod>{AccessMethod::WasapiShared, AccessMethod::WasapiExclusive,
                                                 AccessMethod::DirectKs};

    std::vector<std::string> csvLines;
    csvLines.push_back(
        "renderDevice,captureDevice,accessMethod,requestedBufferFrames,effectiveLatencyMs,"
        "xrunCount,cpuPercent,vacMsPerInt");
    JsonValue jsonResults = JsonValue::MakeArray();
    const std::string renderNameUtf8 = WideToUtf8(renderNameSubstr);
    const std::string captureNameUtf8 = WideToUtf8(captureNameSubstr);

    // gap 9 (VAC 側、実験的機能): --vac-ms-per-int が指定されていなければ
    // 従来どおり単一パス(サイズ 1 の {-1} = 「該当なし」)で回す。
    std::vector<int> vacSweepValues = vacMsPerIntValues.empty() ? std::vector<int>{-1} : vacMsPerIntValues;

    for (int vacMsPerInt : vacSweepValues) {
        if (vacMsPerInt >= 0) {
            std::wprintf(L"[vac] line %d: Milliseconds per interrupt = %d へ設定中"
                        L"(実験的機能、実機未検証)\n",
                        vacLineNumber, vacMsPerInt);
            std::wstring vacErr;
            if (!vac_registry::WriteMsPerInt(vacLineNumber, (DWORD)vacMsPerInt, &vacErr)) {
                std::wprintf(L"[vac] レジストリ書き込み失敗、この設定値をスキップ: %s\n", vacErr.c_str());
                continue;
            }
            if (!vac_registry::RestartDriverService(&vacErr)) {
                std::wprintf(L"[vac] ドライバ再起動失敗、この設定値をスキップ"
                            L"(VAC Control Panel の \"Restart Driver\" を手動で実行し、"
                            L"改めてこの値だけで再実行してください): %s\n",
                            vacErr.c_str());
                continue;
            }
            DWORD readBack = 0;
            if (!vac_registry::ReadMsPerInt(vacLineNumber, &readBack, &vacErr) ||
                (int)readBack != vacMsPerInt) {
                std::wprintf(L"[vac] 読み戻し値が書き込み値と一致しない"
                            L"(read=%lu, want=%d)、この設定値をスキップ\n",
                            readBack, vacMsPerInt);
                continue;
            }
        }

        for (AccessMethod method : methods) {
            for (uint32_t bufferFrames : bufferSizes) {
                DeviceStreamConfig config;
                config.channels = 1;
                config.sampleRate = 48000.0;
                config.preferredBufferFrames = (long)bufferFrames;
                config.aggressiveLowLatency = true;
                // gap 9: 排他モードでは共有エンジンの小バッファ要求パスを
                // 使わない(意味が異なる、engine/device/wasapi_device.cpp の
                // Open() 参照)。exclusiveMode 自体が唯一のモード切り替えなので
                // aggressiveLowLatency は無視されるが、明示的に false にして
                // おく方が意図が伝わる。
                if (method == AccessMethod::WasapiExclusive) {
                    config.aggressiveLowLatency = false;
                    config.exclusiveMode = true;
                }

                std::unique_ptr<IAudioDevice> renderDevice, captureDevice;
                if (method == AccessMethod::WasapiShared || method == AccessMethod::WasapiExclusive) {
                    renderDevice = std::make_unique<wasapi::WasapiDevice>(renderId, false);
                    captureDevice = std::make_unique<wasapi::WasapiDevice>(captureId, true);
                } else if (method == AccessMethod::WasapiSharedLoopback) {
                    // 同じ renderId を 2 つの独立した WASAPI クライアントで開く
                    // (通常のレンダーと、loopback capture)。シェアードモードは
                    // 複数クライアントの同時オープンを許すのでこれで成立する。
                    renderDevice = std::make_unique<wasapi::WasapiDevice>(renderId, false);
                    captureDevice = std::make_unique<wasapi::WasapiDevice>(renderId, /*isCapture=*/true,
                                                                            /*loopback=*/true);
                } else {
                    // DirectKS は WasapiDevice とは別列挙(SetupDiGetClassDevs)なので、
                    // 名前一致で該当フィルタを探す。
                    std::vector<ks::KsDeviceInfo> ksDevices = ks::EnumerateKsAudioDevices();
                    ks::KsDeviceInfo renderInfo, captureInfo;
                    bool foundRender = false, foundCapture = false;
                    for (const auto& d : ksDevices) {
                        if (!foundRender &&
                            d.friendlyName.find(renderNameSubstr) != std::wstring::npos) {
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

                char vacLabel[16];
                if (vacMsPerInt >= 0) {
                    std::snprintf(vacLabel, sizeof(vacLabel), "%d", vacMsPerInt);
                } else {
                    vacLabel[0] = '\0';
                }

                char line[512];
                if (r.ok) {
                    std::snprintf(line, sizeof(line), "%s,%s,%ls,%u,%.3f,%llu,%.2f,%s",
                                  renderNameUtf8.c_str(), captureNameUtf8.c_str(),
                                  AccessMethodName(method), bufferFrames, r.effectiveLatencyMs,
                                  (unsigned long long)r.xrunCount, r.cpuPercent, vacLabel);

                    // --json (gap 8): 失敗した組み合わせは含めない
                    // (engine/ipc/latency_db.h::LookupMeasuredLatencyMs は
                    // xrunCount==0 の測定だけを見るので、失敗行を混ぜても実害は
                    // 無いが、意味のある measuredLatencyMs が無いので素直に除外する)。
                    JsonValue entry = JsonValue::MakeObject();
                    entry["renderDevice"] = renderNameUtf8;
                    entry["captureDevice"] = captureNameUtf8;
                    entry["accessMethod"] = WideToUtf8(AccessMethodName(method));
                    entry["requestedBufferFrames"] = (int)bufferFrames;
                    entry["measuredLatencyMs"] = r.effectiveLatencyMs;
                    entry["xrunCount"] = (double)r.xrunCount;
                    entry["cpuPercent"] = r.cpuPercent;
                    if (vacMsPerInt >= 0) entry["vacMsPerInt"] = vacMsPerInt;
                    jsonResults.Push(entry);
                } else {
                    std::snprintf(line, sizeof(line), "%s,%s,%ls,%u,FAILED,,,%s",
                                  renderNameUtf8.c_str(), captureNameUtf8.c_str(),
                                  AccessMethodName(method), bufferFrames, vacLabel);
                }
                csvLines.push_back(line);
                std::wprintf(L"%hs\n", line);
            }
        }
    }  // for (vacMsPerInt : vacSweepValues)  (gap 9: VAC 内部設定スイープ)

    if (!csvPath.empty()) {
        std::wofstream out(csvPath);
        for (const auto& l : csvLines) out << l.c_str() << L"\n";
    }
    if (!jsonPath.empty()) {
        // std::ofstream の wstring パス受け取りは MSVC の拡張(csvPath の
        // std::wofstream と同じ理由でこのファイルは MSVC 専用)。中身は
        // UTF-8 の JSON テキストなのでバイト列として書けばよく、
        // std::wofstream(ワイド文字ストリーム)ではなく素の std::ofstream を使う。
        std::ofstream out(jsonPath);
        out << jsonResults.Dump();
    }

    return 0;
}
