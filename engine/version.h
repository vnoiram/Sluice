#pragma once
// version.h : バージョン情報(実装ガイド §5.8)
//
// エンジン実行ファイルの Win32 リソース(sluice-engine.rc)とアプリ内の
// ログ/UI 表示の両方から、この 1 箇所を参照するようにする(二重管理を
// 避けるため、.rc 側の数値もここと手動で一致させること。CMake 側での
// 自動生成は今後の課題)。

namespace version {

constexpr int kMajor = 0;
constexpr int kMinor = 1;
constexpr int kPatch = 0;

constexpr const char* kString = "0.1.0";
constexpr const wchar_t* kStringW = L"0.1.0";

}  // namespace version
