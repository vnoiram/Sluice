#pragma once
// vb_cable.h : VB-CABLE 仮想デバイス検出(実装ガイド §5.6)
//
// エンジンから見れば VB-CABLE はただの WASAPI デバイスなので、追加実装は
// 「名前で見つけて仮想入力/仮想出力としてリストに載せる」だけで済む
// (実装ガイド §5.6: 「エンジンから見ればただの WASAPI デバイスなので
// 追加実装はほぼゼロ」)。
//
// VB-CABLE 自体はここでは同梱・自動ダウンロードしない(ライセンス上、
// 公式サイトからのユーザー自身のインストールが必要)。見つからなかった
// 場合の「公式サイトからのインストールが必要です」という案内表示は
// UI 側(実装ガイド §5.7 以降)の責務であり、ここでは行わない。
//
// 命名の向きに注意(混乱しやすい):
//   VB-CABLE の "CABLE Output"(Windows 上は録音デバイス)からは、他の
//   アプリが送った音を「エンジンが」キャプチャできる。つまりエンジンに
//   とっての仮想入力はこちら。
//   VB-CABLE の "CABLE Input"(Windows 上は再生デバイス)へ「エンジンが」
//   書き込むと、他のアプリが CABLE Output からそれを録音できる。つまり
//   エンジンにとっての仮想出力はこちら。

#include <optional>
#include <string>

#include "device/wasapi_device.h"

namespace wasapi {

struct VbCableEndpoints {
    // "CABLE Output"(録音デバイス)。エンジンが ここから読む = 仮想入力。
    std::optional<EndpointInfo> virtualInput;
    // "CABLE Input"(再生デバイス)。エンジンが ここへ書く = 仮想出力。
    std::optional<EndpointInfo> virtualOutput;
};

namespace detail {
// "CABLE Input"/"CABLE Output" だけでなく、複数インスタンス版(有償)の
// "CABLE-A Input"/"CABLE-A Output" 等も緩く拾えるよう、
// 「"CABLE" で始まり、"Input"/"Output" を含む」でマッチさせる。
inline bool StartsWithCable(const std::wstring& name) {
    const std::wstring prefix = L"CABLE";
    return name.size() >= prefix.size() &&
           name.compare(0, prefix.size(), prefix) == 0;
}
}  // namespace detail

// WASAPI デバイス列挙(EnumerateEndpoints)から VB-CABLE らしきデバイスを
// 検出する。見つからなければ対応する optional は空のまま。
inline VbCableEndpoints DetectVbCable() {
    VbCableEndpoints result;

    for (const auto& ep : EnumerateEndpoints(/*isCapture=*/true)) {
        if (detail::StartsWithCable(ep.name) && ep.name.find(L"Output") != std::wstring::npos) {
            result.virtualInput = ep;
            break;
        }
    }
    for (const auto& ep : EnumerateEndpoints(/*isCapture=*/false)) {
        if (detail::StartsWithCable(ep.name) && ep.name.find(L"Input") != std::wstring::npos) {
            result.virtualOutput = ep;
            break;
        }
    }
    return result;
}

}  // namespace wasapi
