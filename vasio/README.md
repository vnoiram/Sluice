# vasio — 仮想 ASIO ドライバ(Phase 2)

DAW から「ただの ASIO デバイス」として見える `vasio.dll` を提供する。実体は
engine プロセスとの橋渡しでしかなく、カーネルコードではないユーザーモード
COM DLL(実装ガイド §8.1)。engine プロセスが未接続の間は無音を返し続け、
接続が来ると共有メモリ越しにオーディオを交換する。

## 必要なもの

- Windows 10/11 x64
- Visual Studio 2022(C++ デスクトップ開発ワークロード)
- CMake 3.25+
- **ASIO SDK**(Steinberg 開発者サイトから無償入手。ライセンス同意が必要。
  `engine/README.md` と同じ配置場所 `engine/thirdparty/asiosdk` を共有する。
  SDK のソースはリポジトリにコミットしないこと)

## ビルド

```bat
:: ASIO SDK は engine/thirdparty/asiosdk に配置済みである前提
:: (engine/README.md 参照。vasio と engine で共有する)

cmake -B build -S .
cmake --build build --config Release
```

`engine/thirdparty/asiosdk` が無い場合は `vasio.dll` の実ビルドをスキップし、
`shared_protocol.h` のオフラインテスト(`test_shared_protocol`)のみを実行する
(`BUILD_VASIO_DRIVER` オプション、`engine/CMakeLists.txt` の
`BUILD_ASIO_HOST` と同じ自動フォールバック方式)。

### Windows Docker での自動ビルド/テスト

リポジトリルートの `scripts/build-vasio-in-windows-docker.ps1` を Windows
ホスト側の PowerShell から実行する(`engine/` 向けの
`build-engine-tests-in-windows-docker.ps1` と同じ Dockerfile を再利用)。

## 登録(DAW から見えるようにする)

```bat
:: 管理者権限のコマンドプロンプトで
regsvr32 build\Release\vasio.dll
```

- `regsvr32` は内部で `DllRegisterServer` を呼び、`HKCR\CLSID\{...}` と
  `HKLM\SOFTWARE\ASIO\Sluice Virtual ASIO` の両方を登録する
  (`common/register.cpp` の `RegisterAsioDriver`、実装ガイド §8.1 手順2)。
- 32bit DAW から見えるようにするには 32bit 版 `vasio.dll` を別途ビルドし、
  32bit 版 `regsvr32`(`%SystemRoot%\SysWOW64\regsvr32.exe`)で登録する。
  現状の CMakeLists.txt は単一アーキテクチャのみ(64bit)を想定しており、
  32bit 対応は将来課題。
- 登録解除は `regsvr32 /u build\Release\vasio.dll`。

## CLSID について

`vasio_driver.h` の `CLSID_SluiceVasio` は開発用の仮 GUID
(`{A1B2C3D4-1234-4E56-8F9A-0123456789AB}`)。実装ガイド §12
「プロジェクト名は GitHub / Google / ドメイン / J-PlatPat(商標クラス9)で
衝突確認してから確定する」に対応する正式なプロジェクト名が決まった時点で、
新規に GUID を採番し直すこと(Windows の `guidgen` 相当のツールで生成する。
既存の CLSID を使い回して公開してはならない)。

## 共有メモリプロトコル

`shared_protocol.h` に定義。engine プロセス側のコンシューマ実装は
`engine/device/vasio_bridge_device.h/.cpp`(`--vasio` フラグで
`sluice-engine.exe` から起動時に接続する、実装ガイド §8.1 手順3)。
`vasio.dll` は「エンジンが未接続でも DAW を待たせずに動く」設計(実装ガイド
§8.1 手順5)なので、この2つは独立にビルド・検証でき、どちらが先に起動しても
(`CreateFileMappingW` が後発側では既存マッピングを返すだけになるよう
コーディングしてある)正しく接続する。Windows 実機・実 DAW での接続確認は
未実施(`engine/README.md` の既知の簡略化を参照)。

- マッピング名 `Local\SluiceVasio.0`、準備完了イベント名
  `Local\SluiceVasioReady.0`(いずれも `vasio::MappingName()`/
  `vasio::ReadyEventName()`)
- 方向は「ToEngine」(DAW→エンジン)/「FromEngine」(エンジン→DAW)で命名し、
  `engine/device/vb_cable.h` にあるような「入力/出力」の命名反転による混乱を
  避けている
- 既定 8in/8out 固定、単一インスタンスのみ(複数 CLSID を跨いだ同時起動は
  将来課題)

## 既知の簡略化(このフェーズの意図的な割り切り)

- `ASIOSTFloat32LSB` 固定、timeInfo モード非対応(`future()` は
  `kAsioCanTimeInfo` に対して非対応を返す)
- サンプルレート変換は一切行わない(DAW 側の要求バッファサイズとエンジン側
  ブロックサイズの差はエンジン境界の ASRC が吸収する設計だが、その ASRC
  自体は engine 側の統合作業でありこのフェーズには含まれない)
- `engine` 側の共有メモリコンシューマ(`engine/device/vasio_bridge_device.h`)
  は実装済みだが、Windows 実機・実 DAW でのロード・接続確認は未実施
  (実機での DAW 互換性確認は REAPER → Cubase → Ableton → OBS-ASIO の順、
  実装ガイド §8.1「テスト」)
- エンジン側からの `kAsioResetRequest` 送出(レート/バッファサイズ変更を
  DAW に伝える経路)は未実装。`ResetPending` を書き込む側の実装が無い
- 32bit DAW 対応、複数インスタンス対応は将来課題

## 注意

本コードはリファレンス実装であり、`engine/` 側と同様に環境(ASIO SDK の
バージョン、DAW ごとの ASIO ホスト実装の癖)によって微修正が必要になる
可能性がある。COM ボイラープレート(`CFactoryTemplate`/`CUnknown`/
`CClassFactory`)は ASIO SDK 付属の `common/combase.cpp`・`common/dllentry.cpp`
をそのままリンクしており、driver/asiosample/asiosmpl.cpp と同じ土台を使う
(自作していない)。
