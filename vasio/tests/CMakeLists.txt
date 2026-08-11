# shared_protocol.h はプラットフォーム非依存(<atomic>/<cstdint> のみ)なので、
# Windows Docker を経由せずここでビルド・実行できる(engine/tests/CMakeLists.txt
# と同じ「純粋ロジックはオフラインでテストする」方針)。

add_executable(test_shared_protocol test_shared_protocol.cpp)
target_compile_features(test_shared_protocol PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(test_shared_protocol PRIVATE /W4 /permissive-)
endif()
add_test(NAME test_shared_protocol COMMAND test_shared_protocol)
