cmake_minimum_required(VERSION 3.25)
project(sluice-vasio CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# vasio.dll は DAW にロードされる仮想 ASIO ドライバ(実装ガイド §8.1)。
# ASIO SDK は不要(../asio-abi/ の独自 ABI 実装を使う。asio-abi/README.md
# 参照)。COM は Windows API なので Windows 以外では自動的にスキップする。
option(BUILD_VASIO_DRIVER "Build the virtual ASIO driver DLL (Windows only)" ON)
option(BUILD_TESTS "Build offline regression tests (shared memory layout)" ON)

set(ASIO_ABI_DIR "${CMAKE_SOURCE_DIR}/../asio-abi")

if(BUILD_VASIO_DRIVER AND NOT WIN32)
    message(WARNING "vasio.dll is Windows-only (COM). Skipping on this platform.")
    set(BUILD_VASIO_DRIVER OFF)
endif()

if(BUILD_VASIO_DRIVER)
    add_library(vasio SHARED
        vasio_driver.cpp
        vasio.def
        "${ASIO_ABI_DIR}/com_server.cpp"
        "${ASIO_ABI_DIR}/asio_registry.cpp"
    )

    target_include_directories(vasio PRIVATE
        "${CMAKE_SOURCE_DIR}"
    )

    target_link_libraries(vasio PRIVATE
        ole32 oleaut32 uuid user32 advapi32
    )

    # ASIO SDK の common/register.cpp(ANSI 専用 API 前提)を使っていた頃は
    # UNICODE/_UNICODE を定義できなかったが、asio-abi/asio_registry.cpp は
    # ワイド文字版 API で統一しているためこの制約は無い。engine/CMakeLists.txt
    # の sluice-engine ターゲットと同じ定義で揃える。
    target_compile_definitions(vasio PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
    )

    if(MSVC)
        target_compile_options(vasio PRIVATE /W4 /permissive-)
    endif()
endif()

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)

    # asio-abi/tests は Windows の COM 型(REFIID/HRESULT 等)に依存するため
    # Windows でのみビルドする(asio-abi/tests/CMakeLists.txt 参照)。
    if(WIN32)
        add_subdirectory("${ASIO_ABI_DIR}/tests" "${CMAKE_BINARY_DIR}/asio_abi_tests")
    endif()
endif()
