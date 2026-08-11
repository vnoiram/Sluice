cmake_minimum_required(VERSION 3.25)
project(sluice-latencybench CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# latencybench.exe は実デバイス I/O(WasapiDevice/KsDevice)を必要とするため
# Windows のみ。xcorr.h のオフラインテストはプラットフォーム非依存なので
# BUILD_TESTS は常に有効にできる(engine/CMakeLists.txt の
# BUILD_ASIO_HOST/BUILD_TESTS 分離と同じ方針)。
option(BUILD_LATENCYBENCH "Build the latencybench executable (Windows only)" ON)
option(BUILD_TESTS "Build offline regression tests (xcorr)" ON)

set(ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../engine")

if(BUILD_LATENCYBENCH AND NOT WIN32)
    message(WARNING "latencybench.exe requires WasapiDevice/KsDevice (Windows only). Skipping on this platform.")
    set(BUILD_LATENCYBENCH OFF)
endif()

if(BUILD_LATENCYBENCH)
    add_executable(latencybench
        main.cpp
        "${ENGINE_DIR}/device/wasapi_device.cpp"
        "${ENGINE_DIR}/device/ks_device.cpp"
    )

    target_include_directories(latencybench PRIVATE
        "${ENGINE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    target_link_libraries(latencybench PRIVATE
        ole32 avrt mmdevapi setupapi ksuser
    )

    target_compile_definitions(latencybench PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
    )

    if(MSVC)
        target_compile_options(latencybench PRIVATE /W4 /permissive-)
    endif()
endif()

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
