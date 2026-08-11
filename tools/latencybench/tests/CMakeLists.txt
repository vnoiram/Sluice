# xcorr.h(信号生成+相互相関)はプラットフォーム非依存(Windows API にも
# ASIO SDK にも依存しない)なので、Windows Docker を経由せずここでビルド・
# 実行できる。この計画で唯一自動実行可能な latencybench のテスト
# (実装ガイド §7.3)。

add_executable(test_xcorr test_xcorr.cpp)
target_compile_features(test_xcorr PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(test_xcorr PRIVATE /W4 /permissive-)
endif()
add_test(NAME xcorr COMMAND test_xcorr)
