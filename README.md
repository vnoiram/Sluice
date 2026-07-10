# Sluice

Windows 向け音声ルーティング/ミキサーアプリ。Voicemeeter 相当の機能に加え、
「複数 ASIO デバイスの同時利用」「入出力数の無制限」を目指す OSS。

## 現在の状況: Phase 1(ミキサーエンジン実装中)

`engine/` に、実装ガイド §5 の構成要素(デバイス抽象化・エンジングラフ・
DSP・IPC)を一通り実装済み。`ui/` に最小限の WPF コントロール UI がある。

- **device/**: `IAudioDevice` インターフェースと、その実装
  `AsioDevice`(実 ASIO パススルー、Phase 0 の成果を移植)・
  `WasapiDevice`(共有モード capture/render)・
  `ProcessLoopbackDevice`(プロセス別ループバックキャプチャ)・
  VB-CABLE 仮想デバイス検出。
- **graph/**: ストリップ/バスの N×M ルーティング行列、RCU 方式の
  トポロジ差し替え(`GraphHandle`)。
- **dsp/**: クロックドリフト補正(ASRC + PI 制御)、4 バンド EQ、
  ゲート、コンプレッサ、リミッタ、メータリング。
- **ipc/**: 名前付きパイプでの JSON-RPC 的制御 API。
- **ui/SluiceUi**: システムトレイ常駐 + 設定ウィンドウ(WPF)。
  `SluiceUi.Core` の `EngineClient` で engine の IPC に接続する。

**未統合の部分**: 上記はそれぞれ単体ではビルド・テスト済みだが、
`engine/main.cpp` は今も Phase 0 のシンプルな 1 対 1 ASIO パススルー
デモのままで、graph/ipc を使った「実際に動くミキサーアプリ」への
配線はまだ行っていない(実機での複数デバイス統合テストが必要なため)。

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
Dockerfile.engine.windows   engine/ のビルド/テスト用 Windows コンテナイメージ
Dockerfile.ui.windows       ui/ のビルド/テスト用 Windows コンテナイメージ(dotnet SDK)
scripts/           リポジトリルートから叩く Windows Docker 起動スクリプト
```

将来的に `vasio/`(仮想 ASIO ドライバ, Phase 2)、`kmdriver/`(カーネル
仮想オーディオデバイス, Phase 3)を追加していく想定。

## ビルド・テスト

### engine/: ASIO SDK ありの実ビルド(Windows, 実機)

`engine/README.md` を参照。ASIO SDK は Steinberg のライセンス上リポジトリに
コミットできないため、各自 `engine/thirdparty/asiosdk` に配置する。

### engine/: コア回帰テストのみ(ASIO SDK 不要)

WSL 等で Windows のビルドツールが手元にない場合、Windows ホスト側の
Docker Desktop(Windows コンテナモード)を使う:

```powershell
# Windows PowerShell から
.\scripts\build-engine-tests-in-windows-docker.ps1
```

`Dockerfile.engine.windows` で VS Build Tools + CMake + vcpkg/libsamplerate
を用意したイメージをビルドし、`docker run` でソースをマウントして
`engine\scripts\run-tests.ps1` を実行する。ASIO SDK が
`engine/thirdparty/asiosdk` にあれば実 ASIO ビルドまで、無ければコア
テストのみ自動でフォールバックする。

### ui/: WPF アプリのビルド確認 + IPC クライアントのテスト

```powershell
# Windows PowerShell から
.\scripts\build-ui-in-windows-docker.ps1
```

`Dockerfile.ui.windows`(dotnet SDK)で `SluiceUi`(WPF)のコンパイル確認と、
`SluiceUi.Core.Tests`(名前付きパイプでの実結合テスト)を実行する。WPF
アプリ自体の実行・見た目の確認は、Windows デスクトップ環境がある実機で
行う必要がある(コンテナにはデスクトップが無い)。

## ライセンス・商標に関する注意

- **ASIO SDK** は Steinberg のライセンスに同意して各自取得すること。SDK
  ソースの再配布・リポジトリへのコミットは禁止されている。
- "ASIO is a trademark and software of Steinberg Media Technologies GmbH"。
- **VB-CABLE** は同梱・自動ダウンロードしない。ユーザーに公式サイトからの
  インストールを案内するのみ(実装ガイド §5.6)。
- 詳細な設計判断・フェーズ計画はローカルの `docs/` 配下に置いているが、
  個人の作業メモを含むためリポジトリには含めていない(`.gitignore` 対象)。
