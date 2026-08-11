# tools/devprobe — デバイス能力プローブ CLI(実装ガイド §2.5)

WASAPI/DirectKS の各デバイスに `IAudioDevice::Probe()`(実装ガイド §5.1・
§5.2.1)を呼び、64 サンプル対応可否・推奨レーン(RT/Compat)・周期情報を
一覧表示する。実際に `Open()`/`Start()` はしない軽量な事前確認ツール。

ASIO は対象外(`tools/latencybench` と同じ、WASAPI/KS だけの軽量ツールに
留める方針。ASIO SDK が不要になったこと自体はもう理由にならないが、
`asio-abi/` を使った ASIO 列挙対応は本ツールでは未着手)。ASIO デバイスの
能力確認は `sluice-engine.exe --list`(`engine/main.cpp`)を使うこと。

## ビルド

```bat
cmake -B build -S .
cmake --build build --config Release
```

`engine/device/wasapi_device.cpp`・`engine/device/ks_device.cpp` を直接
ソースリストに含める(共有ライブラリ化はしない。`tools/latencybench` と
同じ方針)。engine 本体からは `engine/CMakeLists.txt` が
`add_subdirectory` で取り込むため、engine のビルド設定に含めて自動的に
ビルドされる。

## 使い方

```bat
build\Release\devprobe.exe
```

引数は取らない。接続されている全 WASAPI エンドポイント・DirectKS デバイスと、
VB-CABLE/VAC の検出結果を出力して終了する。

## 検証状況

他の Windows 専用コードと同じ制約で、この Linux/WSL 開発環境では実機・
Windows SDK ヘッダでのコンパイル確認ができていない。
