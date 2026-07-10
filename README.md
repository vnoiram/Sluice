# Sluice

Windows 向け音声ルーティング/ミキサーアプリ。Voicemeeter 相当の機能に加え、
「複数 ASIO デバイスの同時利用」「入出力数の無制限」を目指す OSS。

## 現在の状況: Phase 0(エンジン最小実装)

現在は `engine/` にエンジンプロセスの最小実装がある。ASIO デバイス A(入力)
の音を ASIO デバイス B(出力)へ、クロックドリフト補正(ASRC + PI 制御)
付きで流すだけの構成で、複数 ASIO デバイスを同時に開き長時間グリッチなく
動かせることを検証する段階(このプロジェクト最大の技術リスク)。

詳細は [`engine/README.md`](engine/README.md) を参照。

## リポジトリ構成

```
engine/            コア(現状: device/dsp/rt のみ。graph/ipc は Phase 1 以降)
  device/          デバイス抽象化(AsioDevice)
  dsp/             ASRC ラッパ + ドリフト補正 PI コントローラ
  rt/              SPSC リング等、RT スレッド間通信のロックフリー基盤
  tests/           プラットフォーム非依存コアの回帰テスト(ASIO SDK 不要)
  scripts/         Windows Docker コンテナ内で実行するビルド/テストスクリプト
Dockerfile.engine.windows   engine/ のビルド/テスト用 Windows コンテナイメージ
scripts/           リポジトリルートから叩く Windows Docker 起動スクリプト
```

将来的に `vasio/`(仮想 ASIO ドライバ, Phase 2)、`kmdriver/`(カーネル
仮想オーディオデバイス, Phase 3)、`ui/`(UI プロセス)を追加していく想定。

## ビルド・テスト

### ASIO SDK ありの実ビルド(Windows, 実機)

`engine/README.md` を参照。ASIO SDK は Steinberg のライセンス上リポジトリに
コミットできないため、各自 `engine/thirdparty/asiosdk` に配置する。

### コア回帰テストのみ(ASIO SDK 不要)

`engine/rt/spsc_ring.h` と `engine/dsp/drift.h` はプラットフォーム非依存
なので、ASIO SDK なしでもビルド・テストできる。

WSL 等で Windows のビルドツールが手元にない場合、Windows ホスト側の
Docker Desktop(Windows コンテナモード)を使う:

```powershell
# Windows PowerShell から
.\scripts\build-engine-tests-in-windows-docker.ps1
```

`Dockerfile.engine.windows` で VS Build Tools + CMake + vcpkg/libsamplerate
を用意したイメージをビルドし、`docker run` でソースをマウントして
`engine\scripts\run-tests.ps1` を実行する(`BUILD_ASIO_HOST=OFF` でコア
テストのみ)。

## ライセンス・商標に関する注意

- **ASIO SDK** は Steinberg のライセンスに同意して各自取得すること。SDK
  ソースの再配布・リポジトリへのコミットは禁止されている。
- "ASIO is a trademark and software of Steinberg Media Technologies GmbH"。
- 詳細な設計判断・フェーズ計画はローカルの `docs/` 配下に置いているが、
  個人の作業メモを含むためリポジトリには含めていない(`.gitignore` 対象)。
