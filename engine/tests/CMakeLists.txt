# コア(プラットフォーム非依存)部分の回帰テスト。
# ASIO SDK にも Windows API にも依存しない(rt/spsc_ring.h / dsp/drift.h のみ使用)ため、
# sluice-engine.exe がビルドできない環境(ASIO SDK 未配置時)でも常に実行できる。

add_executable(test_spsc_ring test_spsc_ring.cpp)
target_include_directories(test_spsc_ring PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_spsc_ring PRIVATE Threads::Threads)
if(MSVC)
    target_compile_options(test_spsc_ring PRIVATE /W4 /permissive-)
endif()
add_test(NAME spsc_ring COMMAND test_spsc_ring)

add_executable(test_drift test_drift.cpp)
target_include_directories(test_drift PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_drift PRIVATE SampleRate::samplerate Threads::Threads)
if(MSVC)
    target_compile_options(test_drift PRIVATE /W4 /permissive-)
endif()
add_test(NAME drift_convergence COMMAND test_drift)

# エンジングラフ(strip/bus/N×M ルーティング/RCU グラフ差し替え)のオフライン
# 回帰テスト。ASIO SDK にも Windows API にも依存しない(engine/graph/ と
# engine/dsp/drift.h のみ使用)。
# WASAPI 共有モード周期選択(engine/device/wasapi_period.h)のオフライン
# 回帰テスト(実装ガイド §5.2.1)。Windows API にも ASIO SDK にも依存しない。
add_executable(test_wasapi_period_selection test_wasapi_period_selection.cpp)
target_include_directories(test_wasapi_period_selection PRIVATE ${CMAKE_SOURCE_DIR})
if(MSVC)
    target_compile_options(test_wasapi_period_selection PRIVATE /W4 /permissive-)
endif()
add_test(NAME wasapi_period_selection COMMAND test_wasapi_period_selection)

add_executable(test_engine_graph test_engine_graph.cpp)
target_include_directories(test_engine_graph PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_engine_graph PRIVATE SampleRate::samplerate Threads::Threads)
if(MSVC)
    target_compile_options(test_engine_graph PRIVATE /W4 /permissive-)
endif()
add_test(NAME engine_graph COMMAND test_engine_graph)

# DSP モジュール(biquad/EQ/gate/compressor/limiter)の単体回帰テスト
# (実装ガイド §5.5)。ASIO SDK にも Windows API にも依存しない。
add_executable(test_dsp_modules test_dsp_modules.cpp)
target_include_directories(test_dsp_modules PRIVATE ${CMAKE_SOURCE_DIR})
if(MSVC)
    target_compile_options(test_dsp_modules PRIVATE /W4 /permissive-)
endif()
add_test(NAME dsp_modules COMMAND test_dsp_modules)

# IPC 用の最小 JSON 実装(engine/ipc/json_value.h)の回帰テスト
# (実装ガイド §5.6)。ASIO SDK にも Windows API にも依存しない。
add_executable(test_json_value test_json_value.cpp)
target_include_directories(test_json_value PRIVATE ${CMAKE_SOURCE_DIR})
if(MSVC)
    target_compile_options(test_json_value PRIVATE /W4 /permissive-)
endif()
add_test(NAME json_value COMMAND test_json_value)

# デバイス状態→JSON 変換(engine/ipc/device_report.h)のオフライン回帰テスト
# (実装ガイド §5.6)。DeviceStatus/DeviceCaps は Windows API に依存しない。
add_executable(test_device_report_json test_device_report_json.cpp)
target_include_directories(test_device_report_json PRIVATE ${CMAKE_SOURCE_DIR})
if(MSVC)
    target_compile_options(test_device_report_json PRIVATE /W4 /permissive-)
endif()
add_test(NAME device_report_json COMMAND test_device_report_json)

# 名前付きパイプ IPC サーバ(engine/ipc/pipe_server.h)の実結合テスト
# (実装ガイド §5.6)。名前付きパイプはオーディオデバイス/サービス不要の
# 基本 OS 機能なので、WasapiDevice 系と違いここは実際に ctest で実行する。
if(WIN32)
    add_executable(test_ipc_pipe test_ipc_pipe.cpp)
    target_include_directories(test_ipc_pipe PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(test_ipc_pipe PRIVATE Threads::Threads)
    target_compile_definitions(test_ipc_pipe PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
    )
    if(MSVC)
        target_compile_options(test_ipc_pipe PRIVATE /W4 /permissive-)
    endif()
    add_test(NAME ipc_pipe COMMAND test_ipc_pipe)
    # PipeServer::Stop() の既知の制限(クライアントが ReadFile でブロック
    # したまま切断しないと返らない)を踏んだ場合に CI がハングし続けない
    # よう、タイムアウトで確実に終わらせる。
    set_tests_properties(ipc_pipe PROPERTIES TIMEOUT 30)
endif()

# WasapiDevice / ProcessLoopbackDevice はコア回帰テストとは別に、
# コンパイル/リンクできることだけを検証する(ASIO SDK 非依存、実 WASAPI
# 呼び出しは行わない)。
#
# 注意: この実行ファイルは ProcessLoopbackDevice が使う
# ActivateAudioInterfaceAsync を静的リンクしているため、実行時に
# Mmdevapi.dll のロードが必要になる。Windows Server Core ベースの
# ビルドコンテナにはオーディオサブシステムが無く Mmdevapi.dll 自体が
# 存在しないため、この exe は「ビルド(コンパイル+リンク)できること」の
# 確認までに留め、ctest では実行しない(実機では問題なく起動する)。
if(WIN32)
    add_executable(test_wasapi_compile
        test_wasapi_compile.cpp
        ${CMAKE_SOURCE_DIR}/device/wasapi_device.cpp
        ${CMAKE_SOURCE_DIR}/device/process_loopback_device.cpp
    )
    target_include_directories(test_wasapi_compile PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(test_wasapi_compile PRIVATE ole32 avrt mmdevapi)
    target_compile_definitions(test_wasapi_compile PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
    )
    if(MSVC)
        target_compile_options(test_wasapi_compile PRIVATE /W4 /permissive-)
    endif()
    # 意図的に add_test() しない(上記コメント参照)。ビルドの成否だけで
    # コンパイル/リンク確認としては十分。
endif()

# KsDevice(DirectKS バックエンド、実装ガイド §6)も WasapiDevice と同じ理由で
# コンパイル/リンクできることだけを検証する。SetupDiGetClassDevs 自体は
# Windows コンテナでも動作しうるが、実 KS 対応デバイスが無い環境では空リストが
# 返るだけで意味のある検証にならないため、test_wasapi_compile と同じ扱いで
# add_test() はしない。
if(WIN32)
    add_executable(test_ks_compile
        test_ks_compile.cpp
        ${CMAKE_SOURCE_DIR}/device/ks_device.cpp
    )
    target_include_directories(test_ks_compile PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(test_ks_compile PRIVATE setupapi ksuser)
    target_compile_definitions(test_ks_compile PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
    )
    if(MSVC)
        target_compile_options(test_ks_compile PRIVATE /W4 /permissive-)
    endif()
    # 意図的に add_test() しない(上記コメント参照)。
endif()

# クラッシュダンプ収集(engine/crash/crash_handler.h)の実結合テスト
# (実装ガイド §5.7)。子プロセスを意図的にクラッシュさせ、.dmp が実際に
# 書き出されることを確認する。オーディオデバイス/サービス不要なので
# Windows コンテナでも実行できる。
if(WIN32)
    add_executable(crash_handler_test_helper
        crash_handler_test_helper.cpp
        ${CMAKE_SOURCE_DIR}/crash/crash_handler.cpp
    )
    target_include_directories(crash_handler_test_helper PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(crash_handler_test_helper PRIVATE dbghelp)
    target_compile_definitions(crash_handler_test_helper PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
    )

    add_executable(test_crash_handler test_crash_handler.cpp)
    target_include_directories(test_crash_handler PRIVATE ${CMAKE_SOURCE_DIR})
    target_compile_definitions(test_crash_handler PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
    )
    if(MSVC)
        target_compile_options(test_crash_handler PRIVATE /W4 /permissive-)
    endif()
    add_dependencies(test_crash_handler crash_handler_test_helper)
    add_test(NAME crash_handler COMMAND test_crash_handler)
    set_tests_properties(crash_handler PROPERTIES TIMEOUT 30)
endif()
