cmake_minimum_required(VERSION 3.25)
project(sluice-devprobe CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# devprobe.exe は実デバイス列挙(WasapiDevice/KsDevice)を必要とするため
# Windows のみ(tools/latencybench と同じ方針)。プラットフォーム非依存の
# 内部ロジックが無いため、こちらのプロジェクトに BUILD_TESTS は無い。
option(BUILD_DEVPROBE "Build the devprobe executable (Windows only)" ON)

set(ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../engine")

if(BUILD_DEVPROBE AND NOT WIN32)
    message(WARNING "devprobe.exe requires WasapiDevice/KsDevice (Windows only). Skipping on this platform.")
    set(BUILD_DEVPROBE OFF)
endif()

if(BUILD_DEVPROBE)
    add_executable(devprobe
        main.cpp
        "${ENGINE_DIR}/device/wasapi_device.cpp"
        "${ENGINE_DIR}/device/ks_device.cpp"
    )

    target_include_directories(devprobe PRIVATE
        "${ENGINE_DIR}"
    )

    target_link_libraries(devprobe PRIVATE
        ole32 avrt mmdevapi setupapi ksuser
    )

    target_compile_definitions(devprobe PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
    )

    if(MSVC)
        target_compile_options(devprobe PRIVATE /W4 /permissive-)
    endif()
endif()
