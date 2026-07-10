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
