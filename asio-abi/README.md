# asio-abi — ASIO SDK 非依存の独自 ABI 実装

`engine/`(ASIO ホスト)と `vasio/`(仮想 ASIO ドライバ)がこれまで
Steinberg の ASIO SDK(非再配布、`.gitignore` 対象)に依存していた部分の
置き換え。SDK を一切使わずに、実在の ASIO ドライバ/DAW と相互運用できる
ABI(vtable レイアウト・構造体のフィールド配置・定数の数値)だけを独自に
実装している。

## なぜこれが成立するか

`IASIO` インターフェースの vtable レイアウト・構造体のメモリ配置・
定数の数値は、ASIO ドライバとホストが相互運用するために合意しなければ
ならない「事実(ABI)」であり、Steinberg のヘッダファイルの文章表現
(コメント・説明文・コードの構成)そのものとは別物である。
[wineasio](https://github.com/wineasio/wineasio) をはじめ複数の OSS
プロジェクトが、SDK のソースを一切使わずに独自ヘッダでこの ABI を再現し、
実在の ASIO ドライバ/DAW と相互運用できている。本ディレクトリはこれと
同じ考え方を採る(実装ガイド §12 が「GPL にするならクリーンルーム ASIO
ヘッダ方式を検討」として既に言及していた選択肢でもある)。

**正確性の担保方法**: 開発時にはローカルへ実 SDK を配置し(`.gitignore`
対象、リポジトリには含まれない)、メソッドの宣言順・構造体のフィールド
配置・定数の数値がこのディレクトリの実装と一致するかを事実として確認して
いる。ただし文章・コメント・ファイル構成・コードの書き方は独自に書き
下ろしたものであり、SDK のテキストをそのまま転記したものではない。

## 構成

- `asio_abi.h` — `IASIO` インターフェース宣言(メソッドの宣言順が
  vtable レイアウトそのものなので、一切変更しないこと)、使用する構造体
  (`ASIOBufferInfo`/`ASIOChannelInfo`/`ASIOCallbacks`/`ASIOClockSource`
  など)、typedef、定数(`ASIOTrue`/`ASE_OK`/`ASIOSTFloat32LSB`/
  `kAsioResetRequest` など)。`engine/device/asio_host.h`(ホスト側)・
  `vasio/vasio_driver.h`(ドライバ側)の両方がこの 1 ファイルを参照する。
- `com_server.h`/`.cpp` — 単一 CLSID の in-proc COM サーバの最小実装
  (`vasio.dll` が以前 ASIO SDK の `common/combase.cpp`・
  `common/dllentry.cpp` からリンクしていた `CUnknown`/`CFactoryTemplate`/
  `CClassFactory`/`DllGetClassObject`/`DllCanUnloadNow` の代替)。
- `asio_registry.h`/`.cpp` — ASIO ドライバの COM 登録/解除に使う
  レジストリ操作(`vasio.dll` が以前 ASIO SDK の `common/register.cpp` から
  リンクしていた `RegisterAsioDriver`/`UnregisterAsioDriver` の代替)。
  ASIO 固有のロジックは無く、素の Win32 レジストリ API 呼び出しのみ。

## 商標について

"ASIO" is a trademark and software of Steinberg Media Technologies GmbH.
本ディレクトリは Steinberg 社によって提供・承認・監修されたものではない、
独立した ABI 互換実装である。

## 変更時の注意

`asio_abi.h` の `IASIO` のメソッド宣言順・各構造体のフィールド順・
`#pragma pack(push, 4)` は ABI そのものなので、一切変更しないこと
(並べ替え・追加・削除はすべて実在のドライバ/DAW との互換性を壊す)。
新しい ASIO 機能(timeInfo モード、DSD 対応など)を使いたくなった場合は、
既存の宣言順を保ったまま「使っていなかった構造体のフィールドを実際に
読み書きし始める」形で拡張すること。
