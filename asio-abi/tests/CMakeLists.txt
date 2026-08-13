# com_server.h/.cpp は Windows の COM 型(REFIID/HRESULT/IUnknown 等)に
# 依存するため、この tests/CMakeLists.txt は Windows でのみ呼ばれる
# (vasio/CMakeLists.txt の BUILD_TESTS ブロックが WIN32 のときだけ
# add_subdirectory する)。

add_executable(test_com_server
    test_com_server.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../com_server.cpp"
)
target_include_directories(test_com_server PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
)
target_link_libraries(test_com_server PRIVATE ole32 oleaut32 uuid)
target_compile_definitions(test_com_server PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
)
target_compile_features(test_com_server PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(test_com_server PRIVATE /W4 /permissive-)
endif()
add_test(NAME test_com_server COMMAND test_com_server)
