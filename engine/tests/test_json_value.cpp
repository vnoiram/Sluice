// test_json_value.cpp : IPC 用の最小 JSON 実装(engine/ipc/json_value.h)の
// 回帰テスト。プラットフォーム非依存。

#include "ipc/json_value.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

void Fail(const std::string& what) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    std::exit(1);
}

void TestRoundTrip() {
    JsonValue obj = JsonValue::MakeObject();
    obj["id"] = 42;
    obj["name"] = std::string("strip \"1\"\nline2");
    obj["enabled"] = true;
    obj["missing"] = nullptr;
    obj["gainDb"] = -6.5;

    JsonValue arr = JsonValue::MakeArray();
    arr.Push(1);
    arr.Push(2);
    arr.Push(3);
    obj["routing"] = arr;

    const std::string dumped = obj.Dump();
    JsonValue parsed = JsonValue::Parse(dumped);

    if (parsed.At("id").AsInt() != 42) Fail("round-trip: id mismatch");
    if (parsed.At("name").AsString() != "strip \"1\"\nline2")
        Fail("round-trip: name (with quote/newline) mismatch");
    if (parsed.At("enabled").AsBool() != true) Fail("round-trip: enabled mismatch");
    if (!parsed.At("missing").IsNull()) Fail("round-trip: missing should be null");
    if (std::fabs(parsed.At("gainDb").AsNumber() - (-6.5)) > 1e-9)
        Fail("round-trip: gainDb mismatch");
    if (parsed.At("routing").Items().size() != 3) Fail("round-trip: routing array size");
    if (parsed.At("routing").Items()[1].AsInt() != 2) Fail("round-trip: routing[1] mismatch");

    std::printf("PASS: json round-trip (%s)\n", dumped.c_str());
}

void TestNestedObjectsAndWhitespace() {
    // 手書きの(空白多め・キー順序違い)JSON をパースできることを確認する。
    const std::string text =
        "{ \"method\" : \"set_param\" , \"params\" : { \"stripIndex\": 2, "
        "\"gainDb\": 3 } , \"id\": 7 }";
    JsonValue v = JsonValue::Parse(text);
    if (v.At("method").AsString() != "set_param") Fail("nested: method mismatch");
    if (v.At("id").AsInt() != 7) Fail("nested: id mismatch");
    if (v.At("params").At("stripIndex").AsInt() != 2) Fail("nested: stripIndex mismatch");
    if (std::fabs(v.At("params").At("gainDb").AsNumber() - 3.0) > 1e-9)
        Fail("nested: gainDb mismatch");
    std::printf("PASS: json nested objects + whitespace tolerance\n");
}

void TestMalformedThrows() {
    bool threw = false;
    try {
        JsonValue::Parse("{ this is not json");
    } catch (const std::exception&) {
        threw = true;
    }
    if (!threw) Fail("malformed: expected an exception for invalid JSON");
    std::printf("PASS: json malformed input throws\n");
}

}  // namespace

int main() {
    TestRoundTrip();
    TestNestedObjectsAndWhitespace();
    TestMalformedThrows();
    std::printf("ALL PASS: json_value\n");
    return 0;
}
