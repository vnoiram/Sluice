# engine — EngineGraph ベースのミキサーエンジン

ASIO / WASAPI / DirectKS / プロセスループバック / VB-CABLE / VAC を同時に開き、
`graph/engine_graph.h` の N×M ルーティング行列(ストリップ→バス、実装ガイド
§5.4)へ配線するコンソールアプリ。Voicemeeter 相当のミキサーアプリの
エンジンプロセス本体(実装ガイド M1)。

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
:: 全バックエンドのデバイス一覧を表示(index は各バックエンド内で独立)
build\Release\sluice-engine.exe --list

:: 例: ASIO ドライバ 0 を入力、ASIO ドライバ 2 を出力にして起動
:: (--auto-route を付けないと全ルーティングgainはミュートのままなので、
::  音を出すには起動後に set_param で routingGain を設定するか、
::  お試し用に --auto-route を付ける)
build\Release\sluice-engine.exe --asio-in 0 --asio-out 2 --auto-route

:: 例: WASAPI マイク(capture index 0)を ASIO 出力(driver 1)へ、
::     64 サンプル低遅延要求つきで
build\Release\sluice-engine.exe --wasapi-in 0 --asio-out 1 --low-latency --auto-route

:: 例: VB-CABLE を仮想入力として使う(要 VB-CABLE インストール済み)
build\Release\sluice-engine.exe --vbcable-in --asio-out 0 --auto-route
```

CLI オプション一覧:

| オプション | 意味 |
|---|---|
| `--list` | 開かずにデバイス一覧(ASIO/WASAPI/KS/VB-CABLE/VAC)を表示して終了 |
| `--asio-in`/`--asio-out <idx>` | ASIO ドライバを入力/出力として追加(複数回指定可、同一ドライバの二重指定は不可) |
| `--wasapi-in`/`--wasapi-out <idx>` | WASAPI エンドポイントを追加(`--list` の index) |
| `--ks-in`/`--ks-out <idx>` | DirectKS デバイスを追加(実機未検証、実装ガイド §6 参照) |
| `--loopback-pid <pid>` | 指定 PID(と子プロセス)の再生音をキャプチャ入力として追加 |
| `--vbcable-in`/`--vbcable-out` | VB-CABLE を自動検出して追加(未インストールならエラー終了) |
| `--vac-line <n>` | VAC の Line N を自動検出して追加(capture/render 両方あれば両方) |
| `--channels <n>` | デバイスへ要求するチャンネル数(既定 2。WASAPI はミックスフォーマット優先で無視される) |
| `--low-latency` | 実装ガイド §5.2.3 の「積極的低遅延モード」をオプトイン(WASAPI に 64 サンプル要求 + RAW モード) |
| `--auto-route` | 全ストリップを全バスへ 0dB で送る(既定は全ルーティングgainがミュート) |

起動時と、デバイスリセット時・1 秒ごとの統計に、各デバイスの xrun・実効
レイテンシ・レーンが出る:

```
devices=2 strips=2 buses=2
  [0] asio/in "Focusrite Scarlett 2i2"  xrun=0  latency=1.33ms  lane=RT
  [1] asio/out "Focusrite Scarlett 2i2" *master*  xrun=0  latency=1.33ms  lane=RT
```

IPC(名前付きパイプ `\\.\pipe\sluice-engine`、実装ガイド §5.6)で
`get_devices`・`get_topology`・`set_param`(strip/bus のゲイン・ミュート・
ソロ・ルーティング・EQ/ゲート/コンプ/リミッタ)・`add_strip`・
`remove_strip` が呼べる。`devices_changed` push 通知も届く。

## 合格基準(実装ガイド §4.4、Phase 0 PoC 由来。マルチデバイス構成にも適用される)

1. 4 時間連続で収束後の xrun = 0
2. ASRC 比が一定値に収束
3. サイン波を入力しループバック録音した波形に不連続なし
4. 出力デバイスのコンパネでバッファサイズ変更 → 自動復帰(kAsioResetRequest 相当)
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
  master_clock.h    マスタークロック候補選定(RT Lane のみ、実装ガイド §2.3)。
                    main.cpp の OpenAndBuild() から実際に呼ばれる
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

## 既知の簡略化(意図的な割り切り)

- デバイス構成はプロセス起動時の CLI 引数で固定する。実行中に新規デバイスを
  追加する IPC は無い(`add_strip`/`remove_strip` は「既に開いている
  デバイスのチャンネル」に対するストリップの増減のみ)
- マスタークロック以外の出力デバイスはドリフト補正されない
  (`InputBoundary`/ASRC は入力側にしか無い、`main.cpp` 冒頭コメント参照)。
  複数の出力デバイスを使う構成では、マスター以外の出力デバイスのクロックが
  ずれると長時間で xrun しうる。対称な「出力バウンダリ」の実装は将来課題
- サンプル型は Int32LSB / Float32LSB のみ対応(他はエラー表示)
- kAsioResetRequest(および WASAPI/KS の resetRequested)は「全体を作り直す」
  最単純対応。個々のデバイスだけを差し替える部分再構築は行わない
- ドライバ操作は main スレッド(STA)で実施。製品版ではドライバごとの
  管理スレッドに分離する(実装ガイド §4.1.2)
- KS バックエンドは Windows 実機でのコンパイル・動作検証が未実施
  (`device/ks_device.h` 冒頭コメント参照)

## 注意

本コードはリファレンス実装であり、環境(ASIO SDK のバージョン、ドライバの癖)
によって微修正が必要になる可能性がある。特に `device/asio_host.cpp` 冒頭の
コメントに記載した「コールバック・トランポリン」の仕組みを理解してから
読み進めること。
