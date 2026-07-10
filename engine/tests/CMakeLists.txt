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
