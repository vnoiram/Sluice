#pragma once
// crash_handler.h : クラッシュダンプ収集(実装ガイド §5.8)
//
// 「クラッシュダンプ収集(MiniDumpWriteDump)とバージョン情報を最初から
// 入れておく」に対応。アプリ起動時に Install() を一度呼ぶだけでよい。
// 以後、未処理例外(SEH レベル。std::terminate 経由のC++例外は対象外)が
// 発生すると、指定ディレクトリ(既定は %TEMP%)へ .dmp ファイルを書き出し
// てからプロセスを終了する。
//
// 既知の割り切り: MiniDumpNormal(スタック+モジュール一覧程度)のみ。
// ヒープ全体を含むフルダンプは大きくなりすぎるため対象外。

#include <string>

namespace crashhandler {

// dumpDirectory が空文字列なら %TEMP% を使う。末尾の '\' は有無どちらでもよい。
void Install(const std::wstring& dumpDirectory = L"");

}  // namespace crashhandler
