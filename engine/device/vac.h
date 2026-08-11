#pragma once
// vac.h : VAC (Virtual Audio Cable) 仮想デバイス検出(実装ガイド §7.1/§7.2)
//
// エンジンから見れば VAC もただの WASAPI デバイスなので、追加実装は
// 「名前で見つけてリストに載せる」だけで済む(実装ガイド §7.2「エンジンから
// 見ればただの WASAPI/KS デバイスなので追加実装はほぼゼロ」)。
//
// vb_cable.h との違い(なぜ別ファイルにしたか):
//   VB-CABLE は単一の input/output ペア+命名反転という特殊な構造なのに対し、
//   VAC は最大 256 本の "Line N" ペアを持てる(実装ガイド §7.1「最大 256
//   ケーブル」)。1 つの optional<pair> では表現できず、vector<VacLine> が
//   自然な構造になるため、vb_cable.h を汎用化するのではなく並列のファイルに
//   分けている。
//
// VAC 自体はここでは同梱・自動ダウンロードしない(ライセンス上、公式サイト
// からのユーザー自身のインストールが必要、実装ガイド §7.2)。見つからなかった
// 場合の案内表示は UI 側(実装ガイド §5.6 以降)の責務であり、ここでは行わない。
//
// 罠: 実装ガイド本文が挙げる命名例は "Line 1 (Virtual Audio Cable)" の
// 1 例のみで、capture 側/render 側で表記が変わる(例: "(Output)"/"(Input)"
// サフィックスの有無)かどうかは実機での確認が必要(未検証)。ここでは
// "Line " + 数字 の前方一致のみで判定し、capture/render は同じ行番号を
// EnumerateEndpoints(isCapture=true/false) の両方から独立に探して
// マージする(vb_cable.h の isCapture ごとの走査と同じ方針)。

#include <cwctype>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "device/wasapi_device.h"

namespace wasapi {

struct VacLine {
    int lineNumber = 0;
    std::optional<EndpointInfo> capture;  // エンジンにとっての仮想入力
    std::optional<EndpointInfo> render;   // エンジンにとっての仮想出力
};

namespace detail {

// "Line " で始まる名前から行番号を抽出する。パースできなければ -1。
// 例: "Line 1 (Virtual Audio Cable)" → 1
inline int ParseVacLineNumber(const std::wstring& name) {
    const std::wstring prefix = L"Line ";
    if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) return -1;

    size_t pos = prefix.size();
    size_t digitsEnd = pos;
    while (digitsEnd < name.size() && iswdigit(name[digitsEnd])) ++digitsEnd;
    if (digitsEnd == pos) return -1;  // 数字が続かない

    // "Line 1 (Virtual Audio Cable)" のように、数字の直後に区切り
    // (スペース等)があることを緩く確認する(誤検出を避けるため、数字の
    // 直後が英数字の場合は別の名前とみなして弾く)。
    if (digitsEnd < name.size() && iswalnum(name[digitsEnd])) return -1;

    return std::stoi(name.substr(pos, digitsEnd - pos));
}

}  // namespace detail

// WASAPI デバイス列挙(EnumerateEndpoints)から VAC らしきデバイスを検出し、
// 行番号でグルーピングして返す(行番号の昇順)。
inline std::vector<VacLine> DetectVac() {
    std::map<int, VacLine> byLine;

    for (const auto& ep : EnumerateEndpoints(/*isCapture=*/true)) {
        const int line = detail::ParseVacLineNumber(ep.name);
        if (line < 0) continue;
        byLine[line].lineNumber = line;
        byLine[line].capture = ep;
    }
    for (const auto& ep : EnumerateEndpoints(/*isCapture=*/false)) {
        const int line = detail::ParseVacLineNumber(ep.name);
        if (line < 0) continue;
        byLine[line].lineNumber = line;
        byLine[line].render = ep;
    }

    std::vector<VacLine> result;
    result.reserve(byLine.size());
    for (auto& [line, vacLine] : byLine) result.push_back(std::move(vacLine));
    return result;
}

}  // namespace wasapi
