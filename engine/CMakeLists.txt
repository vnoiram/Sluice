cmake_minimum_required(VERSION 3.25)
project(sluice-engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# BUILD_ASIO_HOST: 実機 ASIO パススルー(sluice-engine.exe)。ASIO SDK(Steinberg, 非再配布)が要る。
# BUILD_TESTS    : プラットフォーム非依存のコア(リング/ASRC/ドリフト)の回帰テスト。
#                  ASIO SDK なしでもここは常にビルドできる(実装ガイド §8)。
option(BUILD_ASIO_HOST "Build the ASIO passthrough executable (requires ASIO SDK)" ON)
option(BUILD_TESTS "Build core regression tests (ring buffer, ASRC/drift)" ON)

find_package(SampleRate CONFIG REQUIRED)  # vcpkg: libsamplerate
find_package(Threads REQUIRED)

# --- ASIO SDK -----------------------------------------------------------
# SDK は各自 Steinberg からダウンロードし thirdparty/asiosdk に配置する。
# ここではヘッダ(iasiodrv.h, asio.h 系)のみ参照し、SDK の .cpp は使わない。
set(ASIOSDK_DIR "${CMAKE_SOURCE_DIR}/thirdparty/asiosdk")

if(BUILD_ASIO_HOST AND NOT EXISTS "${ASIOSDK_DIR}/common/iasiodrv.h")
    message(WARNING
        "ASIO SDK not found (expected ${ASIOSDK_DIR}/common/iasiodrv.h). "
        "Skipping sluice-engine.exe target; core regression tests still build. See README.md")
    set(BUILD_ASIO_HOST OFF)
endif()

if(BUILD_ASIO_HOST)
    add_executable(sluice-engine
        main.cpp
        device/asio_host.cpp
        device/wasapi_device.cpp
        device/process_loopback_device.cpp
        crash/crash_handler.cpp
        sluice-engine.rc
    )

    target_include_directories(sluice-engine PRIVATE
        "${ASIOSDK_DIR}/common"
        "${CMAKE_SOURCE_DIR}"
    )

    target_link_libraries(sluice-engine PRIVATE
        SampleRate::samplerate
        ole32 user32 advapi32 winmm avrt mmdevapi dbghelp
    )

    target_compile_definitions(sluice-engine PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
    )

    if(MSVC)
        target_compile_options(sluice-engine PRIVATE /W4 /permissive-)
    endif()
endif()

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
