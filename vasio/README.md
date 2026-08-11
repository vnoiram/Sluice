# vasio — 仮想 ASIO ドライバ(Phase 2)

DAW から「ただの ASIO デバイス」として見える `vasio.dll` を提供する。実体は
engine プロセスとの橋渡しでしかなく、カーネルコードではないユーザーモード
COM DLL(実装ガイド §8.1)。engine プロセスが未接続の間は無音を返し続け、
接続が来ると共有メモリ越しにオーディオを交換する。

## 必要なもの

- Windows 10/11 x64
- Visual Studio 2022(C++ デスクトップ開発ワークロード)
- CMake 3.25+

ASIO SDK は不要。[`../asio-abi/`](../asio-abi/README.md)(リポジトリ直下、
`engine/` と共通)の独自 ABI 実装(Steinberg の SDK を使わないクリーンルーム
実装)でビルドする。

## ビルド

```bat
cmake -B build -S .
cmake --build build --config Release
```

`vasio.dll` の実ビルドと `shared_protocol.h` のオフラインテスト
(`test_shared_protocol`)を常に両方行う(`BUILD_VASIO_DRIVER` オプションは
Windows 以外で自動的に OFF になる。`engine/CMakeLists.txt` の
`BUILD_ASIO_HOST` と同じ方式)。

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
  (`../asio-abi/asio_registry.h` の `RegisterAsioDriver`、実装ガイド §8.1 手順2)。
- gap 11: 32bit DAW から見えるようにするには 32bit 版 `vasio.dll` を別途
  ビルドし、32bit 版 `regsvr32`(`%SystemRoot%\SysWOW64\regsvr32.exe`)で
  登録する。`CMakeLists.txt` 自体はアーキテクチャに依存しない(レジストリ
  登録も `KEY_WOW64_*` を使わず、呼び出し元プロセス(`regsvr32.exe` /
  `regsvr32.exe` の SysWOW64 版)のビットネスに自然に追従する、
  `../asio-abi/asio_registry.cpp` 参照)ため、32bit 版のビルドは
  `scripts/build-vasio-x86-in-windows-docker.ps1`(`-A Win32` で configure
  するだけ)で行える。64bit 版と 32bit 版は別々の CLSID/レジストリキーを
  共有する設計ではない点に注意(同じ `CLSID_SluiceVasio` を両方に登録して
  問題ないが、64bit 版と 32bit 版を同時に別々の instanceId で使う場合は
  各プロセス側で `SLUICE_VASIO_INSTANCE` を正しく設定すること、下記参照)。
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

- マッピング名 `Local\SluiceVasio.<instanceId>`、準備完了イベント名
  `Local\SluiceVasioReady.<instanceId>`(いずれも `vasio::MappingName()`/
  `vasio::ReadyEventName()`、既定 instanceId は `"0"`)
- 方向は「ToEngine」(DAW→エンジン)/「FromEngine」(エンジン→DAW)で命名し、
  `engine/device/vb_cable.h` にあるような「入力/出力」の命名反転による混乱を
  避けている
- 既定 8in/8out 固定
- gap 11: **複数インスタンス対応**。1 つの `sluice-engine.exe` プロセスで
  `--vasio --vasio-instance <id>` を複数回指定すると、instanceId ごとに
  別々の共有メモリ/イベントで独立した vasio ブリッジを開ける。DAW 側
  (`vasio.dll` がロードされるプロセス)は環境変数 `SLUICE_VASIO_INSTANCE`
  に同じ `<id>` を設定してから起動する必要がある(vasio.dll 自体は CLI
  引数を持たないため)。`<id>` を指定しない場合は両者とも既定の `"0"` を
  使い、これは複数インスタンス対応導入前の固定名と完全に一致する
  (後方互換)。Windows 実機・複数 DAW プロセスでの実接続確認は未実施。

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
  DAW に伝える経路)は実装済み(`VasioBridgeDevice::RequestDawReset()`、
  `engine/main.cpp` の監視ループがマスタークロックのブロックサイズ変化を
  検出したときに呼ぶ)。Windows 実機・実 DAW での動作確認は未実施
- 32bit DAW 対応はビルド面(`scripts/build-vasio-x86-in-windows-docker.ps1`)
  では対応済み(上記参照)。複数インスタンス対応(現状 instanceId 固定
  "0" の共有メモリ命名を拡張する必要がある)は将来課題のまま。

## 注意

本コードはリファレンス実装であり、`engine/` 側と同様に環境(DAW ごとの
ASIO ホスト実装の癖)によって微修正が必要になる可能性がある。COM
ボイラープレート(単一 CLSID 用クラスファクトリ、`DllGetClassObject`/
`DllCanUnloadNow`)は ASIO SDK を使わず
[`../asio-abi/com_server.h`](../asio-abi/com_server.h) の独自実装を使う
(`driver/asiosample/asiosmpl.cpp` を当初の構成の参考にしたが、SDK のコードは
一切リンクしていない)。
