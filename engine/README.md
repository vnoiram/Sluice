# engine — 複数 ASIO デバイス同時駆動 + ドリフト補正パススルー

ASIO デバイス A(入力)の音を ASIO デバイス B(出力)へ、クロックドリフト補正
(ASRC + PI 制御)付きで流すコンソールアプリ。Voicemeeter 相当のミキサー
アプリの土台となるエンジンプロセスの最小実装(Phase 0)。

## 必要なもの

- Windows 10/11 x64
- Visual Studio 2022(C++ デスクトップ開発ワークロード)
- CMake 3.25+
- **ASIO SDK**(Steinberg 開発者サイトから無償入手。ライセンス同意が必要)
- libsamplerate(vcpkg で導入)
- ASIO デバイス 2 つ(片方は FlexASIO で代用可。ただし同一ドライバの
  二重指定は不可)

## セットアップ

```bat
:: 1. ASIO SDK を配置(SDK のソースはリポジトリにコミットしないこと)
::    ダウンロードした asiosdk_2.3.3 等を以下に展開:
::    engine/thirdparty/asiosdk/common/iasiodrv.h が存在する状態にする

:: 2. libsamplerate
vcpkg install libsamplerate:x64-windows

:: 3. ビルド
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

## 実行

```bat
:: ドライバ一覧を表示
build\Release\sluice-engine.exe --list

:: 例: ドライバ 0 を入力、ドライバ 2 を出力にしてパススルー開始
build\Release\sluice-engine.exe --in 0 --out 2
```

実行中は 1 秒ごとに統計行が出る:

```
fill=50.2%  ratio=1.0000132  xrun(in=0 out=0)  cb(A=93750 B=93748)
```

- `fill`: リングバッファ充填率。50% 付近で安定していれば正常
- `ratio`: ドリフト補正比。起動後 30〜60 秒かけて一定値に収束し、
  その値が 2 デバイス間の実クロック差(ppm)を表す
- `xrun`: アンダーラン/オーバーラン回数。**収束後は増えないこと**が合格条件

## 合格基準(実装ガイド §4.4)

1. 4 時間連続で収束後の xrun = 0
2. ratio が一定値に収束
3. サイン波を入力しループバック録音した波形に不連続なし
4. 出力デバイスのコンパネでバッファサイズ変更 → 自動復帰(kAsioResetRequest)
5. RT スレッド内アロケーション 0

## 構成

実装ガイド §2.4 の推奨リポジトリ構成に合わせている(今はまだ device/dsp/rt
のみで、graph/ipc は Phase 1 以降に追加する)。

```
CMakeLists.txt
main.cpp            エントリポイント。列挙・起動・統計表示
device/
  asio_host.h/.cpp  ASIO ドライバのロードと薄いラッパ(要点は .h のコメント参照)
dsp/
  drift.h           PI コントローラ + libsamplerate ラッパ(ASRC)
rt/
  spsc_ring.h       ロックフリー SPSC リングバッファ
tests/
  test_spsc_ring.cpp  SPSC リングのマルチスレッドストレステスト
  test_drift.cpp      FakeDevice によるドリフト補正の回帰テスト(実装ガイド §8)
scripts/
  run-tests.ps1       Windows Docker コンテナ内で cmake configure/build/ctest を実行
```

## コア回帰テスト(ASIO SDK 不要)

`rt/spsc_ring.h` と `dsp/drift.h` は Windows API にも ASIO SDK にも依存しない
プラットフォーム非依存のコードなので、ASIO SDK が手元になくても常にビルド・
実行できる。CMake は `BUILD_ASIO_HOST` / `BUILD_TESTS` の 2 オプションで
`sluice-engine.exe`(実 ASIO パススルー)とテストを分離しており、ASIO SDK
未配置時は自動的に `BUILD_ASIO_HOST=OFF` にフォールバックしてテストのみ
ビルドする。

```bat
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake ^
    -DBUILD_ASIO_HOST=OFF
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

- `test_spsc_ring`: 1 プロデューサ・1 コンシューマで単一要素/可変長バッチの
  Write/Read を大量に流し、順序保証・欠落なし・RT 区間アロケーション 0 件を検証。
- `test_drift`: 実装ガイド §8 の FakeDevice 相当(ドリフト+ジッタを注入した
  仮想デバイス)で、実時間 30 分相当のソークテストを圧縮シミュレーションで
  数十秒に短縮して実行。±400ppm(補正上限 500ppm 未満)のドリフトで収束後
  xrun=0・fill 率が 50% 近辺で安定することを確認する。
  **これは drift.h の収束特性だけを検証するもので、上記合格基準の 3
  (サイン波ループバックの波形不連続チェック)は実オーディオ経路が要るため
  スコープ外(実機での手動検証が必要)。**

### Windows Docker での自動ビルド/テスト

リポジトリルートの `scripts/build-engine-tests-in-windows-docker.ps1` を
Windows ホスト側の PowerShell から実行すると、Docker Desktop(Windows
コンテナモード)で VS Build Tools + CMake + vcpkg/libsamplerate 環境を
用意してビルド・テストまで自動で行う(詳細はリポジトリルートの README
参照)。ASIO SDK は同梱していないため `BUILD_ASIO_HOST=OFF` でコアテスト
のみを回す。

## 既知の簡略化(PoC の意図的な割り切り)

- ステレオ固定(各デバイスの先頭 2ch のみ使用)
- サンプル型は Int32LSB / Float32LSB のみ対応(他はエラー表示)
- kAsioResetRequest は「全体を作り直す」最単純対応
- ドライバ操作は main スレッド(STA)で実施。製品版ではドライバごとの
  管理スレッドに分離する(実装ガイド §4.1.2)

## 注意

本コードはリファレンス実装であり、環境(ASIO SDK のバージョン、ドライバの癖)
によって微修正が必要になる可能性がある。特に `device/asio_host.cpp` 冒頭の
コメントに記載した「コールバック・トランポリン」の仕組みを理解してから
読み進めること。
