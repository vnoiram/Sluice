cmake_minimum_required(VERSION 3.25)
project(sluice-engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# BUILD_ASIO_HOST: 実機 ASIO パススルー(sluice-engine.exe)。ASIO SDK は不要
#                  (asio-abi/ の独自 ABI 実装を使う。asio-abi/README.md 参照)。
#                  COM/Windows API 前提なので Windows 以外では自動的に OFF。
# BUILD_TESTS    : プラットフォーム非依存のコア(リング/ASRC/ドリフト)の回帰テスト。
#                  Windows 以外でもここは常にビルドできる(実装ガイド §10)。
option(BUILD_ASIO_HOST "Build the ASIO passthrough executable (Windows only)" ON)
option(BUILD_TESTS "Build core regression tests (ring buffer, ASRC/drift)" ON)

find_package(SampleRate CONFIG REQUIRED)  # vcpkg: libsamplerate
find_package(Threads REQUIRED)

if(BUILD_ASIO_HOST AND NOT WIN32)
    message(WARNING "sluice-engine.exe requires Windows (COM/WASAPI/KS). Skipping on this platform.")
    set(BUILD_ASIO_HOST OFF)
endif()

if(BUILD_ASIO_HOST)
    add_executable(sluice-engine
        main.cpp
        device/asio_host.cpp
        device/wasapi_device.cpp
        device/process_loopback_device.cpp
        device/ks_device.cpp
        device/vasio_bridge_device.cpp
        rt/rt_alloc_guard.cpp
        crash/crash_handler.cpp
        sluice-engine.rc
    )

    target_include_directories(sluice-engine PRIVATE
        "${CMAKE_SOURCE_DIR}"
    )

    target_link_libraries(sluice-engine PRIVATE
        SampleRate::samplerate
        ole32 user32 advapi32 winmm avrt mmdevapi dbghelp setupapi ksuser
    )

    target_compile_definitions(sluice-engine PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
        # 実装ガイド §3.2「開発ビルドでは RT スレッド内アロケーション検出
        # フックを有効化する」。Debug 構成でのみ engine/rt/rt_alloc_guard.h の
        # グローバル operator new/delete オーバーライドを有効にする
        # (Release では完全に無効化され、オーバーヘッドは無い)。
        $<$<CONFIG:Debug>:SLUICE_RT_ALLOC_GUARD>
    )

    if(MSVC)
        target_compile_options(sluice-engine PRIVATE /W4 /permissive-)
        # 実装ガイド §3.2「Release は /O2 /fp:fast /arch:AVX2」。Release
        # 構成にのみ付ける(Debug は AVX2 を要求しないスカラー経路のまま。
        # graph/simd_mix.h の #if defined(__AVX2__) と対応)。/O2 と /fp:fast
        # は CMake の既定 Release フラグ($<CONFIG:Release>)に既に /O2 相当
        # (/O2)が含まれるため、/fp:fast のみ追加する。
        target_compile_options(sluice-engine PRIVATE
            $<$<CONFIG:Release>:/arch:AVX2 /fp:fast>
        )
    endif()
endif()

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# tools/latencybench(実装ガイド §7.3)はリポジトリルート直下 /tools に
# 独立した CMake プロジェクトとして存在する(§2.5 のレイアウトに合わせる)。
# out-of-tree で取り込むことで、engine/scripts/run-tests.ps1 のようなターゲット
# 名を決め打ちしない汎用 cmake/ctest ドライバから自動的にビルド対象になる。
if(EXISTS "${CMAKE_SOURCE_DIR}/../tools/latencybench/CMakeLists.txt")
    add_subdirectory("${CMAKE_SOURCE_DIR}/../tools/latencybench"
                     "${CMAKE_BINARY_DIR}/tools/latencybench")
endif()

# tools/devprobe(実装ガイド §2.5)も同じ方針で取り込む。
if(EXISTS "${CMAKE_SOURCE_DIR}/../tools/devprobe/CMakeLists.txt")
    add_subdirectory("${CMAKE_SOURCE_DIR}/../tools/devprobe"
                     "${CMAKE_BINARY_DIR}/tools/devprobe")
endif()
