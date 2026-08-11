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
- 測定対象の軸: `accessMethod ∈ {WASAPI 共有, DirectKS}` ×
  `requestedBufferFrames ∈ {64, 128, 256, 512}`。
- **WASAPI 排他モードは現状未対応**(`engine/device/wasapi_device.h` が
  共有モードのみ実装しているため。§7.3 が挙げる測定軸のうち、この部分は
  将来 engine 側に排他モード対応が入ってから追加する)。

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
