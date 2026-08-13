# Sluice

English version: [README.en.md](README.en.md)

Windows 向け音声ルーティング/ミキサーアプリ。Voicemeeter 相当の機能に加え、
「複数 ASIO デバイスの同時利用」「入出力数の無制限」を目指す OSS。

## 現在の状況: Phase 1(ミキサーエンジン実装中)・Phase 1.5/2 一部着手

`engine/` に、実装ガイド §5 の構成要素(デバイス抽象化・エンジングラフ・
DSP・IPC)を一通り実装済み。`ui/` に最小限の WPF コントロール UI がある。
Phase 1.5(KS バックエンド)と Phase 2(仮想 ASIO ドライバ)にも着手済み。

- **device/**: `IAudioDevice` インターフェースと、その実装
  `AsioDevice`(実 ASIO パススルー、Phase 0 の成果を移植)・
  `WasapiDevice`(共有モード capture/render)・
  `ProcessLoopbackDevice`(プロセス別ループバックキャプチャ)・
  `KsDevice`(DirectKS バックエンド、Phase 1.5)・
  VB-CABLE / VAC 仮想デバイス検出。
- **graph/**: ストリップ/バスの N×M ルーティング行列、RCU 方式の
  トポロジ差し替え(`GraphHandle`)。
- **dsp/**: クロックドリフト補正(ASRC + PI 制御)、4 バンド EQ、
  ゲート、コンプレッサ、リミッタ、メータリング。
- **ipc/**: 名前付きパイプでの JSON-RPC 的制御 API。`get_devices` で
  デバイス一覧(レーン・実効レイテンシ・xrun・ASRC 比)を取得でき、
  `devices_changed` push 通知(`PipeServer::Notify()`)で自動更新も届く
  (実装ガイド §5.6)。
- **ui/SluiceUi**: システムトレイ常駐 + 設定ウィンドウ(WPF)。
  `SluiceUi.Core` の `EngineClient` で engine の IPC に接続し、
  デバイス一覧を表示・自動更新する。

**未統合の部分**: `graph/`(エンジングラフ・N×M ルーティング)は単体では
ビルド・テスト済みだが、`engine/main.cpp` は今も Phase 0 のシンプルな
1 対 1 ASIO パススルーデモのままで、`EngineGraph` を使った「実際に動く
ミキサーアプリ」への配線はまだ行っていない(実機での複数デバイス統合
テストが必要なため)。IPC(`get_devices`/push 通知)はこの Phase-0
デバイス(devIn/devOut)に対して既に配線済み — 将来 `EngineGraph` に
統合されても JSON スキーマ(デバイス配列)は変わらない設計。

詳細は [`engine/README.md`](engine/README.md) を参照。

## リポジトリ構成

```
engine/
  device/          デバイス抽象化(IAudioDevice)+ ASIO/WASAPI/ProcessLoopback/VB-CABLE
  graph/           ストリップ/バス/N×M ルーティング/RCU グラフ差し替え
  dsp/             EQ・ゲート・コンプ・リミッタ・メータ・ASRC/ドリフト補正
  ipc/             名前付きパイプ JSON-RPC サーバ
  rt/              SPSC リング等、RT スレッド間通信のロックフリー基盤
  tests/           プラットフォーム非依存コアの回帰テスト(ASIO SDK 不要)
  scripts/         Windows Docker コンテナ内で実行するビルド/テストスクリプト
ui/
  SluiceUi/        WPF アプリ本体(システムトレイ + 設定ウィンドウ)
  SluiceUi.Core/   WPF 非依存の IPC クライアント(EngineClient)
  SluiceUi.Core.Tests/  EngineClient の回帰テスト
  scripts/         Windows Docker コンテナ内で実行するビルド/テストスクリプト
vasio/             仮想 ASIO ドライバ(vasio.dll, Phase 2)。DAW から見える
                   COM ドライバ本体・共有メモリプロトコル・オフラインテスト
                   (詳細は vasio/README.md)。engine 側の共有メモリ
                   コンシューマは engine/device/vasio_bridge_device.h/.cpp
                   に実装済み(Windows実機/実DAWでの接続確認は未実施)。
tools/latencybench/  仮想デバイス実測ベンチマークツール(実装ガイド §7.3)。
                   M 系列再生+相互相関で往復レイテンシを測定する
                   (詳細は tools/latencybench/README.md)。
Dockerfile.engine.windows   engine/ のビルド/テスト用 Windows コンテナイメージ
Dockerfile.ui.windows       ui/ のビルド/テスト用 Windows コンテナイメージ(dotnet SDK)
scripts/           リポジトリルートから叩く Windows Docker 起動スクリプト
```

Phase 3(カーネル仮想オーディオデバイス)のうち、DAW から ASIO 経由で到達する
角度は `engine/device/ks_device.h`(DirectKS バックエンド)が既にカバーして
いる。実装ガイド §1.2 の通り、PortCls ミニポートを正しく書けば WASAPI /
DirectSound / MME / DirectKS のすべてから自動的に見えるため、ASIO ホストは
ASIO4ALL / ASIO2KS と同じ手法(= `KsDevice`)で個別対応なしに到達できる
(実装ガイド §6.2)。将来的に `kmdriver/`(WDM/PortCls ミニポートドライバ
本体)を追加していく想定だが、カーネルドライバの実装自体は現時点では対応
方針を保留しており、着手していない。

## ビルド・テスト

ASIO SDK は不要。`engine/`・`vasio/` とも [`asio-abi/`](asio-abi/README.md)
の独自 ABI 実装(Steinberg の SDK を使わないクリーンルーム実装)を使うため、
外部 SDK の取得・配置なしに `sluice-engine.exe`/`vasio.dll` を実ビルドできる。

### engine/: 実ビルド + コア回帰テスト(Windows)

`engine/README.md` を参照。

WSL 等で Windows のビルドツールが手元にない場合、Windows ホスト側の
Docker Desktop(Windows コンテナモード)を使う:

```powershell
# Windows PowerShell から
.\scripts\build-engine-tests-in-windows-docker.ps1
```

`Dockerfile.engine.windows` で VS Build Tools + CMake + vcpkg/libsamplerate
を用意したイメージをビルドし、`docker run` でソースをマウントして
`engine\scripts\run-tests.ps1` を実行する。`sluice-engine.exe` の実ビルドと
コアテストの両方が常に走る。

### vasio/: 仮想 ASIO ドライバのビルド確認

```powershell
# Windows PowerShell から
.\scripts\build-vasio-in-windows-docker.ps1
```

`Dockerfile.engine.windows` を再利用し、`vasio.dll` の実ビルドと共有メモリ
プロトコルのオフラインテスト(`test_shared_protocol`)の両方を行う。DAW
からの実際のロード確認・`regsvr32` での登録手順は
[`vasio/README.md`](vasio/README.md) 参照(実機での確認が必要、Docker
では検証不可)。

### tools/latencybench/: 仮想デバイス実測ベンチマーク

```powershell
# Windows PowerShell から
.\scripts\build-latencybench-in-windows-docker.ps1
```

`Dockerfile.engine.windows` を再利用する。`latencybench.exe` 本体は Windows
専用ビルドだが、信号生成・相互相関のオフラインテスト(`test_xcorr`)は
プラットフォーム非依存。実デバイスでの使い方は
[`tools/latencybench/README.md`](tools/latencybench/README.md) 参照
(実機での確認が必要、Docker では検証不可)。

### ui/: WPF アプリのビルド確認 + IPC クライアントのテスト

```powershell
# Windows PowerShell から
.\scripts\build-ui-in-windows-docker.ps1
```

`Dockerfile.ui.windows`(dotnet SDK)で `SluiceUi`(WPF)のコンパイル確認と、
`SluiceUi.Core.Tests`(名前付きパイプでの実結合テスト)を実行する。WPF
アプリ自体の実行・見た目の確認は、Windows デスクトップ環境がある実機で
行う必要がある(コンテナにはデスクトップが無い)。

### パッケージング(インストーラ)

`packaging/sluice.iss`(Inno Setup)。ビルド手順・署名(SignPath
Foundation)に関する未実施のアクションアイテムは
[`packaging/README.md`](packaging/README.md) を参照。

## ライセンス・商標に関する注意

- **ASIO SDK は不要**。`engine/`・`vasio/` とも Steinberg の SDK を使わず、
  [`asio-abi/`](asio-abi/README.md) の独自 ABI 実装(クリーンルーム実装、
  wineasio 等と同じ考え方)でビルドする。
- "ASIO is a trademark and software of Steinberg Media Technologies GmbH"。
  `asio-abi/` は Steinberg 社によって提供・承認・監修されたものではない、
  独立した ABI 互換実装である。
- **VB-CABLE** および **VAC (Virtual Audio Cable)** は同梱・自動ダウンロード
  しない。ユーザーに公式サイトからのインストールを案内するのみ
  (実装ガイド §7.2/§12)。
- **SmartScreen について**: `sluice-engine.exe`/`SluiceUi.exe`/インストーラ
  は現時点で未署名(SignPath Foundation への署名申請はリポジトリの公開が
  前提のため未着手。CI は整備済み、`.github/workflows/ci.yml`。
  `packaging/README.md` 参照)。初回起動時に
  Windows SmartScreen が「発行元不明」の警告を出すことがある。その場合は
  「詳細情報」→「実行」を選ぶことで起動できる(自己責任での実行になる点、
  配布時にユーザーへ明示すること)。
- 詳細な設計判断・フェーズ計画はローカルの `docs/` 配下に置いているが、
  個人の作業メモを含むためリポジトリには含めていない(`.gitignore` 対象)。
