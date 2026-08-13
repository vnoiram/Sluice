# format_caps.h(DeviceCaps の整形)は Windows API にも実デバイスにも
# 依存しない(DeviceCaps 自体は engine/device/iaudio_device.h でプラット
# フォーム非依存に定義されている)ので、Windows Docker を経由せずここで
# ビルド・実行できる。tools/latencybench/tests/CMakeLists.txt と同じ方針。

add_executable(test_format_caps test_format_caps.cpp)
target_include_directories(test_format_caps PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${ENGINE_DIR}"
)
target_compile_features(test_format_caps PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(test_format_caps PRIVATE /W4 /permissive-)
endif()
add_test(NAME format_caps COMMAND test_format_caps)
