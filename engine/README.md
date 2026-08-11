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

実装ガイド §2.5 の推奨リポジトリ構成に合わせている。

```
CMakeLists.txt
main.cpp            エントリポイント。列挙・起動・統計表示・IPC 配線
device/
  iaudio_device.h   デバイス抽象化インターフェース。Lane/DeviceCaps/Probe()
                    を含む(実装ガイド §5.1・§2.3)
  asio_host.h/.cpp  IAudioDevice を実装する ASIO ドライバのラッパ(要点は .h のコメント参照)
  wasapi_device.h/.cpp  IAudioDevice を実装する WASAPI 共有モード capture/render
                    (実装ガイド §5.2)。IAudioClient3 の 64 サンプル要求・
                    AUDCLNT_E_ENGINE_PERIODICITY_LOCKED 正常系対応を含む
  wasapi_period.h   周期選択の純粋ロジック(ChoosePeriodFrames、実装ガイド §5.2.1)。
                    Windows API 非依存でオフラインテスト可能
  process_loopback_device.h/.cpp  プロセス別ループバックキャプチャ(実装ガイド §5.3)
  ks_device.h/.cpp  IAudioDevice を実装する DirectKS(Kernel Streaming)バックエンド
                    (実装ガイド §6, Phase 1.5)。素の KS ストリーミング I/O
                    (ReadFile/WriteFile + KSSTREAM_HEADER)を使い、WaveRT には
                    未対応(要点は .h 冒頭の罠コメント参照)。Phase 3(カーネル
                    仮想オーディオデバイス)が将来公開する KS エンドポイントに
                    ASIO4ALL/ASIO2KS と同じ手法で到達できる経路でもある
                    (実装ガイド §1.2・§6.2)。
  vb_cable.h        VB-CABLE 仮想デバイス検出(実装ガイド §7.1/§7.2)
  vac.h             VAC (Virtual Audio Cable) 仮想デバイス検出。最大256本の
                    "Line N" ペアに対応(実装ガイド §7.1/§7.2)
graph/
  engine_graph.h    エンジングラフ本体、InputBoundary、RCU 方式のグラフ差し替え
                    (実装ガイド §5.4)
  strip.h / bus.h   入力ストリップ / 出力バス(N×M ルーティング、実装ガイド §5.4.1)
  param_buffer.h    制御→RT のトリプルバッファ(実装ガイド §5.4.2)
  master_clock.h    マスタークロック候補選定(RT Lane のみ、実装ガイド §2.3)
dsp/
  drift.h           PI コントローラ + libsamplerate ラッパ(ASRC)。Lane 別に
                    SRC_SINC_FASTEST/MEDIUM_QUALITY を選択(実装ガイド §4.3.2)
  biquad.h / eq.h / gate.h / compressor.h / limiter.h / meter.h  DSP モジュール一式(実装ガイド §5.5)
ipc/
  json_value.h      IPC 用の最小 JSON エンコード/デコード(実装ガイド §5.6)
  pipe_server.h     名前付きパイプ JSON-RPC サーバ。get_devices・push 通知
                    (devices_changed)に対応(実装ガイド §5.6)
  device_report.h   DeviceStatus/DeviceCaps → JSON 変換の純粋関数。Windows API
                    非依存でオフラインテスト可能
rt/
  spsc_ring.h       ロックフリー SPSC リングバッファ
crash/
  crash_handler.h/.cpp  クラッシュダンプ収集(実装ガイド §5.7)
version.h            バージョン情報(実装ガイド §5.7)
tests/
  test_spsc_ring.cpp             SPSC リングのマルチスレッドストレステスト
  test_drift.cpp                 FakeDevice によるドリフト補正の回帰テスト(実装ガイド §10)
  test_engine_graph.cpp          N×M ミックス・RCU・マスタークロック選定の回帰テスト
  test_dsp_modules.cpp           DSP モジュール単体テスト
  test_json_value.cpp            JSON エンコード/デコードの回帰テスト
  test_device_report_json.cpp    device_report.h の回帰テスト
  test_wasapi_period_selection.cpp  ChoosePeriodFrames の回帰テスト
  test_ipc_pipe.cpp              名前付きパイプ IPC の実結合テスト(Windows)
  test_wasapi_compile.cpp        WasapiDevice/ProcessLoopbackDevice/VB-CABLE/VAC のコンパイル確認(Windows)
  test_ks_compile.cpp            KsDevice のコンパイル確認(Windows)
  test_crash_handler.cpp         クラッシュダンプ収集の実結合テスト(Windows)
scripts/
  run-tests.ps1       Windows Docker コンテナ内で cmake configure/build/ctest を実行
thirdparty/
  asiosdk/            ASIO SDK(各自配置。.gitignore 対象、リポジトリには含まれない)
```

エンジン内部フォーマットは float32 / プレーナ(チャンネルごとに独立した
`SpscRing<float>`、実装ガイド §2.4)。`IAudioDevice::CaptureRing(ch)` /
`RenderRing(ch)` はチャンネルごとに 1 本のリングを返す。ASRC/ドリフト補正は
デバイス実装の外(エンジン境界 = `main.cpp`)に置き、`AsioDevice` 自身は
「自分の RT スレッドで自分のリングに読み書きするだけ」に責務を絞っている
(実装ガイド §5.1)。

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
- `test_drift`: 実装ガイド §10 の FakeDevice 相当(ドリフト+ジッタを注入した
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
参照)。`BUILD_ASIO_HOST` は CMakeLists.txt の既定(ASIO SDK があれば ON、
なければ自動で OFF にフォールバック)のまま呼んでいるので、
`engine/thirdparty/asiosdk` を配置していれば `sluice-engine.exe` の実ビルド
まで、配置していなければコアテストのみを回す。

## 既知の簡略化(PoC の意図的な割り切り)

- `AsioDevice`/`IAudioDevice` 自体は `DeviceStreamConfig.channels` で任意の
  チャンネル数を扱えるが、`main.cpp` のパススルーデモは現状ステレオ固定
  (`kChannels = 2`)
- サンプル型は Int32LSB / Float32LSB のみ対応(他はエラー表示)
- kAsioResetRequest は「全体を作り直す」最単純対応
- ドライバ操作は main スレッド(STA)で実施。製品版ではドライバごとの
  管理スレッドに分離する(実装ガイド §4.1.2)

## 注意

本コードはリファレンス実装であり、環境(ASIO SDK のバージョン、ドライバの癖)
によって微修正が必要になる可能性がある。特に `device/asio_host.cpp` 冒頭の
コメントに記載した「コールバック・トランポリン」の仕組みを理解してから
読み進めること。
