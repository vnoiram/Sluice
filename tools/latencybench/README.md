# tools/latencybench — 仮想デバイス実測ベンチマーク(実装ガイド §7.3)

VAC / VB-CABLE 等の仮想オーディオケーブルへ M 系列(最大長系列)を再生し、
同じケーブルの録音側から取得した信号との相互相関で往復レイテンシを測定する。
`{device, accessMethod, requestedBufferFrames}` を総当たりし、実効レイテンシ
(ms)・xrun 回数・CPU% を CSV で出力する。

## 必要なもの

- Windows 10/11 x64
- 測定対象の仮想ケーブル(VAC または VB-CABLE)がインストール済みで、
  再生側・録音側の両エンドポイントが Windows に見えていること

## ビルド

```bat
cmake -B build -S .
cmake --build build --config Release
```

`engine/device/wasapi_device.cpp`・`engine/device/ks_device.cpp` を直接
ソースリストに含める(共有ライブラリ化はしない。既存のテストターゲットと
同じ方針)。

## 使い方

```bat
:: 仮想ケーブルのエンドポイントを確認
build\Release\latencybench.exe --list

:: 例: VB-CABLE で総当たり測定し、結果を CSV へ書き出す
build\Release\latencybench.exe --sweep "CABLE Input" "CABLE Output" --csv results.csv
```

- 第1引数(--sweep の後)は再生側(render)エンドポイント名の部分一致、
  第2引数は録音側(capture)エンドポイント名の部分一致。
- 測定対象の軸: `accessMethod ∈ {WASAPI 共有, WASAPI 排他, DirectKS}` ×
  `requestedBufferFrames ∈ {64, 128, 256, 512}`(gap 6/gap 9 の実装により
  WASAPI 排他モードも対応済み)。

## VAC 内部設定の自動スイープ(実験的機能、gap 9)

`--vac-line <N> --vac-ms-per-int <v1,v2,...>` を追加すると、指定した VAC
ケーブル(`--list` が表示する "VAC Line N" の N)のレジストリ値
"Milliseconds per interrupt" を各値へ書き換えながら外側でスイープする。

```bat
build\Release\latencybench.exe --sweep "Line 1" "Line 1" ^
  --vac-line 1 --vac-ms-per-int 2,5,10,20 --csv vac-sweep.csv
```

**要管理者権限。実機の VAC インストールでの動作確認がまだできていない、
実験的機能。** 根拠・未検証事項は `vac_registry.h` のコメントを参照。
実機で試す際は以下を確認し、結果をメンテナに共有してほしい:

1. 管理者として実行しないと `ERROR_ACCESS_DENIED` で全設定値がスキップ
   されるはずだが、実際にそうなるか。
2. `--vac-line` に渡した番号が、VAC Control Panel 上のケーブル番号
   (レジストリの `Cable <N>` の N)と一致して書き込めているか。
   一致しない場合は `reg query "HKLM\SOFTWARE\EuMus Design\Virtual Audio Cable\4"`
   で実際のサブキー名を確認してほしい。
3. サービス経由の再起動(`RestartDriverService`)が失敗する場合、
   `services.msc` で `VirtualAudioCable_*` という名前のサービスが実在するか、
   実在するならその正式名を教えてほしい(現状は前方一致でしか特定できて
   いない)。
4. 再起動が成功したように見えても、実際に "ms per int" が音の遅延に反映
   されているか(`--csv` の `effectiveLatencyMs` が値ごとに単調に変化する
   か)を目視確認してほしい。

## VB-CABLE 内部設定について(未実装、要 reg export 差分)

VB-CABLE の内部レイテンシ/サンプルレートは `VBCABLE_ControlPanel.exe`
(`C:\Program Files\VB\CABLE`、要管理者権限)経由でのみ変更でき、公式に
ドキュメント化されたレジストリキーが見つからなかったため、VAC と違って
当て推量では自動化していない。実機で VB-CABLE を導入済みの場合、以下の
手順でレジストリ差分を取って共有してもらえれば、それを一次情報として
安全に自動スイープを実装できる:

```bat
reg export HKLM\SOFTWARE before.reg
:: ここで VBCABLE_ControlPanel.exe (管理者) で内部レイテンシ/SR を変更
reg export HKLM\SOFTWARE after.reg
fc before.reg after.reg
```

(`HKLM\SOFTWARE` 全体は大きいので、`VB-Audio` 等それらしいサブキーが
分かっていれば範囲を絞ってよい。)

## Windows Docker では実機 I/O 検証ができない

`scripts/build-latencybench-in-windows-docker.ps1` はコンパイル・
`xcorr` のオフラインテストのみを検証する。Windows コンテナには
オーディオドライバのサブシステム自体が無く([Microsoft Learn](
https://learn.microsoft.com/en-us/virtualization/windowscontainers/deploy-containers/hardware-devices-in-containers)
も Hyper-V 分離コンテナでのデバイス割り当て/共有を非サポートと明記)、
VAC/VB-CABLE のようなカーネルモードドライバをコンテナ内にインストール
すること自体ができない。実機 I/O 検証(このセクション・上記の VAC/VB-CABLE
節)は、Docker ではなく素の Windows(このリポジトリを WSL2 で使っている
場合は、その WSL2 が乗っている Windows ホスト本体でよい)上で
`latencybench.exe` を直接実行する必要がある。

`scripts/build-latencybench-in-windows-docker.ps1` でビルドした
`latencybench.exe` は、コンテナのボリュームマウント経由でホスト側にも
そのまま残る(`build\Release\latencybench.exe`)ので、それを Windows
ホストで直接実行すればよい。

## 検証状況

`xcorr.h`(信号生成・相互相関)は Windows API に依存しないため、この
Linux/WSL 開発環境で実際にビルド・実行して検証済み(`tests/test_xcorr.cpp`、
既知の遅延に対するオフセット検出精度・ノイズ耐性を確認)。`main.cpp` 本体
(実デバイス I/O)は他の Windows 専用コードと同じ制約で実機・Windows Docker
でのコンパイル確認ができていない。

## 出力の使い道(実装ガイド §7.3)

1. アプリの推奨構成をドキュメントに書ける(実測に基づく説得力)
2. `DeviceCaps.measuredLatencyMs`(`engine/device/iaudio_device.h`)に流し込んで
   レーン自動判定の根拠になる(現状は手動転記。IPC 経由での自動反映は
   将来課題)
3. Phase 3 で自前ドライバを作るときの目標スペックが定量化される
