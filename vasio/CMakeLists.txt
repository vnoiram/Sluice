cmake_minimum_required(VERSION 3.25)
project(sluice-vasio CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# vasio.dll は DAW にロードされる仮想 ASIO ドライバ(実装ガイド §8.1)。
# engine/CMakeLists.txt の BUILD_ASIO_HOST と同じ理由・同じ流儀で、ASIO SDK
# (Steinberg, 非再配布)が無ければこのターゲットはスキップする。
option(BUILD_VASIO_DRIVER "Build the virtual ASIO driver DLL (requires ASIO SDK)" ON)
option(BUILD_TESTS "Build offline regression tests (shared memory layout)" ON)

set(ASIOSDK_DIR "${CMAKE_SOURCE_DIR}/../engine/thirdparty/asiosdk")

if(BUILD_VASIO_DRIVER AND NOT EXISTS "${ASIOSDK_DIR}/common/iasiodrv.h")
    message(WARNING
        "ASIO SDK not found (expected ${ASIOSDK_DIR}/common/iasiodrv.h). "
        "Skipping vasio.dll target; offline tests still build. See engine/README.md")
    set(BUILD_VASIO_DRIVER OFF)
endif()

if(BUILD_VASIO_DRIVER AND NOT WIN32)
    message(WARNING "vasio.dll is Windows-only (COM). Skipping on this platform.")
    set(BUILD_VASIO_DRIVER OFF)
endif()

if(BUILD_VASIO_DRIVER)
    add_library(vasio SHARED
        vasio_driver.cpp
        vasio.def
        "${ASIOSDK_DIR}/common/combase.cpp"
        "${ASIOSDK_DIR}/common/dllentry.cpp"
        "${ASIOSDK_DIR}/common/register.cpp"
    )

    target_include_directories(vasio PRIVATE
        "${ASIOSDK_DIR}/common"
        "${CMAKE_SOURCE_DIR}"
    )

    target_link_libraries(vasio PRIVATE
        ole32 oleaut32 uuid user32 advapi32
    )

    # 注意: UNICODE/_UNICODE をここで定義しないこと。ASIO SDK 付属の
    # common/register.cpp は char*(ANSI)decl 前提で書かれており
    # (GetModuleHandle(char*) 等)、UNICODE を定義すると型不一致でビルドが
    # 壊れる。engine/CMakeLists.txt の sluice-engine ターゲットとは異なる点。
    target_compile_definitions(vasio PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN
    )

    if(MSVC)
        target_compile_options(vasio PRIVATE /W4 /permissive-)
    endif()
endif()

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
