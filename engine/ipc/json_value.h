#pragma once
// json_value.h : IPC プロトコル用の最小限の JSON エンコード/デコード
// (実装ガイド §5.7)
//
// 「JSON-RPC 的な制御 API」を外部ライブラリを増やさず自前実装する方針
// (軽量、依存を減らす)。フル仕様の JSON パーサではなく、本プロジェクトの
// IPC メッセージ(数値・文字列・真偽値・null・オブジェクト・配列。
// ネストは浅い想定)を表現できれば十分という割り切り。
//
// 制御プレーン専用(RT スレッドからは使わない)なので、内部で
// std::string/std::vector/std::map のアロケーションを行っても構わない。

#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    JsonValue() : type_(Type::Null) {}
    JsonValue(std::nullptr_t) : type_(Type::Null) {}
    JsonValue(bool b) : type_(Type::Bool), bool_(b) {}
    JsonValue(double n) : type_(Type::Number), number_(n) {}
    JsonValue(int n) : type_(Type::Number), number_((double)n) {}
    JsonValue(const std::string& s) : type_(Type::String), string_(s) {}
    JsonValue(const char* s) : type_(Type::String), string_(s) {}

    static JsonValue MakeArray() {
        JsonValue v;
        v.type_ = Type::Array;
        return v;
    }
    static JsonValue MakeObject() {
        JsonValue v;
        v.type_ = Type::Object;
        return v;
    }

    Type GetType() const { return type_; }
    bool IsNull() const { return type_ == Type::Null; }

    // --- Object 操作 ---
    JsonValue& operator[](const std::string& key) {
        type_ = Type::Object;
        return object_[key];
    }
    bool Has(const std::string& key) const {
        return type_ == Type::Object && object_.find(key) != object_.end();
    }
    const JsonValue& At(const std::string& key) const {
        static const JsonValue kNull;
        if (type_ != Type::Object) return kNull;
        auto it = object_.find(key);
        return it != object_.end() ? it->second : kNull;
    }

    // --- Array 操作 ---
    void Push(JsonValue v) {
        type_ = Type::Array;
        array_.push_back(std::move(v));
    }
    const std::vector<JsonValue>& Items() const { return array_; }

    // --- アクセサ ---
    bool AsBool(bool def = false) const { return type_ == Type::Bool ? bool_ : def; }
    double AsNumber(double def = 0.0) const { return type_ == Type::Number ? number_ : def; }
    int AsInt(int def = 0) const { return type_ == Type::Number ? (int)number_ : def; }
    std::string AsString(const std::string& def = "") const {
        return type_ == Type::String ? string_ : def;
    }

    std::string Dump() const {
        std::ostringstream os;
        DumpTo(os);
        return os.str();
    }

    static JsonValue Parse(const std::string& text) {
        size_t pos = 0;
        JsonValue v = ParseValue(text, pos);
        return v;
    }

private:
    void DumpTo(std::ostringstream& os) const {
        switch (type_) {
        case Type::Null:
            os << "null";
            break;
        case Type::Bool:
            os << (bool_ ? "true" : "false");
            break;
        case Type::Number:
            os << number_;
            break;
        case Type::String:
            DumpString(os, string_);
            break;
        case Type::Array: {
            os << '[';
            for (size_t i = 0; i < array_.size(); ++i) {
                if (i) os << ',';
                array_[i].DumpTo(os);
            }
            os << ']';
            break;
        }
        case Type::Object: {
            os << '{';
            bool first = true;
            for (const auto& kv : object_) {
                if (!first) os << ',';
                first = false;
                DumpString(os, kv.first);
                os << ':';
                kv.second.DumpTo(os);
            }
            os << '}';
            break;
        }
        }
    }

    static void DumpString(std::ostringstream& os, const std::string& s) {
        os << '"';
        for (char c : s) {
            switch (c) {
            case '"': os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\n': os << "\\n"; break;
            case '\r': os << "\\r"; break;
            case '\t': os << "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)(unsigned char)c);
                    os << buf;
                } else {
                    os << c;
                }
            }
        }
        os << '"';
    }

    static void SkipWs(const std::string& s, size_t& pos) {
        while (pos < s.size() &&
              (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r'))
            ++pos;
    }

    static JsonValue ParseValue(const std::string& s, size_t& pos) {
        SkipWs(s, pos);
        if (pos >= s.size()) throw std::runtime_error("json: unexpected end of input");
        const char c = s[pos];
        if (c == '{') return ParseObject(s, pos);
        if (c == '[') return ParseArray(s, pos);
        if (c == '"') return JsonValue(ParseString(s, pos));
        if (c == 't') { Expect(s, pos, "true"); return JsonValue(true); }
        if (c == 'f') { Expect(s, pos, "false"); return JsonValue(false); }
        if (c == 'n') { Expect(s, pos, "null"); return JsonValue(nullptr); }
        return ParseNumber(s, pos);
    }

    static void Expect(const std::string& s, size_t& pos, const char* lit) {
        const size_t len = std::strlen(lit);
        if (s.compare(pos, len, lit) != 0) throw std::runtime_error("json: bad literal");
        pos += len;
    }

    static JsonValue ParseObject(const std::string& s, size_t& pos) {
        JsonValue v = MakeObject();
        ++pos;  // '{'
        SkipWs(s, pos);
        if (pos < s.size() && s[pos] == '}') { ++pos; return v; }
        for (;;) {
            SkipWs(s, pos);
            std::string key = ParseString(s, pos);
            SkipWs(s, pos);
            if (pos >= s.size() || s[pos] != ':')
                throw std::runtime_error("json: expected ':'");
            ++pos;
            v.object_[key] = ParseValue(s, pos);
            SkipWs(s, pos);
            if (pos < s.size() && s[pos] == ',') { ++pos; continue; }
            if (pos < s.size() && s[pos] == '}') { ++pos; break; }
            throw std::runtime_error("json: expected ',' or '}'");
        }
        return v;
    }

    static JsonValue ParseArray(const std::string& s, size_t& pos) {
        JsonValue v = MakeArray();
        ++pos;  // '['
        SkipWs(s, pos);
        if (pos < s.size() && s[pos] == ']') { ++pos; return v; }
        for (;;) {
            v.array_.push_back(ParseValue(s, pos));
            SkipWs(s, pos);
            if (pos < s.size() && s[pos] == ',') { ++pos; continue; }
            if (pos < s.size() && s[pos] == ']') { ++pos; break; }
            throw std::runtime_error("json: expected ',' or ']'");
        }
        return v;
    }

    static std::string ParseString(const std::string& s, size_t& pos) {
        if (pos >= s.size() || s[pos] != '"')
            throw std::runtime_error("json: expected string");
        ++pos;
        std::string out;
        while (pos < s.size() && s[pos] != '"') {
            char c = s[pos++];
            if (c == '\\' && pos < s.size()) {
                char e = s[pos++];
                switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    // 簡易対応: \uXXXX は ASCII 範囲のみ厳密に扱う
                    // (それ以外は '?' に落とす。制御用途には十分)。
                    if (pos + 4 <= s.size()) {
                        unsigned code = (unsigned)std::stoul(s.substr(pos, 4), nullptr, 16);
                        pos += 4;
                        out += (code < 128) ? (char)code : '?';
                    }
                    break;
                }
                default:
                    out += e;
                }
            } else {
                out += c;
            }
        }
        if (pos >= s.size()) throw std::runtime_error("json: unterminated string");
        ++pos;  // 閉じの '"'
        return out;
    }

    static JsonValue ParseNumber(const std::string& s, size_t& pos) {
        const size_t start = pos;
        if (pos < s.size() && (s[pos] == '-' || s[pos] == '+')) ++pos;
        while (pos < s.size() &&
              (std::isdigit((unsigned char)s[pos]) || s[pos] == '.' || s[pos] == 'e' ||
               s[pos] == 'E' || s[pos] == '+' || s[pos] == '-'))
            ++pos;
        if (pos == start) throw std::runtime_error("json: expected number");
        return JsonValue(std::stod(s.substr(start, pos - start)));
    }

    Type type_;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<JsonValue> array_;
    std::map<std::string, JsonValue> object_;
};
