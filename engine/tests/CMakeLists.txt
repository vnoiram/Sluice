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

# WasapiDevice はコア回帰テストとは別に、コンパイル/リンクできることだけを
# 検証する(ASIO SDK 非依存、実 WASAPI 呼び出しは行わない)。
# Windows コンテナ等オーディオサービスが無い環境でも安全に実行できるよう、
# テスト本体はシンボル解決の確認のみで実際のデバイス列挙は呼ばない。
if(WIN32)
    add_executable(test_wasapi_compile
        test_wasapi_compile.cpp
        ${CMAKE_SOURCE_DIR}/device/wasapi_device.cpp
    )
    target_include_directories(test_wasapi_compile PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(test_wasapi_compile PRIVATE ole32 avrt)
    target_compile_definitions(test_wasapi_compile PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
    )
    if(MSVC)
        target_compile_options(test_wasapi_compile PRIVATE /W4 /permissive-)
    endif()
    add_test(NAME wasapi_compile COMMAND test_wasapi_compile)
endif()
