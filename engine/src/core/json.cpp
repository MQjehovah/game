#include "neon/core/json.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <functional>

namespace neon::core {
namespace {

struct Parser {
    const std::string& text;
    size_t pos = 0;
    std::string error;

    explicit Parser(const std::string& t) : text(t) {}

    void SkipWs() {
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
    }

    bool Consume(char c) {
        SkipWs();
        if (pos < text.size() && text[pos] == c) {
            ++pos;
            return true;
        }
        return false;
    }

    bool ParseValue(Json& out) {
        SkipWs();
        if (pos >= text.size()) {
            error = "unexpected end of input";
            return false;
        }
        char c = text[pos];
        if (c == '{') return ParseObject(out);
        if (c == '[') return ParseArray(out);
        if (c == '"') return ParseString(out);
        if (c == 't' || c == 'f') return ParseBool(out);
        if (c == 'n') return ParseNull(out);
        if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber(out);
        error = std::string("unexpected character '") + c + "'";
        return false;
    }

    bool ParseObject(Json& out) {
        ++pos; // {
        out = Json();
        out.type_ = Json::Type::Object;
        if (Consume('}')) return true;
        for (;;) {
            SkipWs();
            Json key;
            if (!ParseString(key)) return false;
        if (!Consume(':')) {
            char ctx[32] = {};
            for (int i = 0; i < 24 && pos + i < text.size(); ++i) ctx[i] = text[pos + i];
            error = std::string("expected ':' at pos ") + std::to_string(pos) + " near '" + ctx + "'";
            return false;
        }
            Json value;
            if (!ParseValue(value)) return false;
            out.object_[key.string_] = value;
            if (Consume('}')) return true;
            if (!Consume(',')) {
                error = "expected ',' or '}'";
                return false;
            }
        }
    }

    bool ParseArray(Json& out) {
        ++pos; // [
        out = Json();
        out.type_ = Json::Type::Array;
        if (Consume(']')) return true;
        for (;;) {
            Json value;
            if (!ParseValue(value)) return false;
            out.array_.push_back(value);
            if (Consume(']')) return true;
            if (!Consume(',')) {
                error = "expected ',' or ']'";
                return false;
            }
        }
    }

    bool ParseString(Json& out) {
        ++pos; // "
        out = Json();
        out.type_ = Json::Type::String;
        std::string& s = out.string_;
        while (pos < text.size()) {
            unsigned char c = static_cast<unsigned char>(text[pos]);
            if (c == '"') {
                ++pos;
                return true;
            }
            if (c == '\\') {
                ++pos;
                if (pos >= text.size()) break;
                char e = text[pos++];
                switch (e) {
                    case '"': s += '"'; break;
                    case '\\': s += '\\'; break;
                    case '/': s += '/'; break;
                    case 'b': s += '\b'; break;
                    case 'f': s += '\f'; break;
                    case 'n': s += '\n'; break;
                    case 'r': s += '\r'; break;
                    case 't': s += '\t'; break;
                    case 'u': {
                        uint32_t cp = ParseHex4();
                        if (cp >= 0xD800 && cp <= 0xDBFF && pos + 1 < text.size() &&
                            text[pos] == '\\' && text[pos + 1] == 'u') {
                            pos += 2;
                            uint32_t low = ParseHex4();
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        }
                        AppendUTF8(s, cp);
                        break;
                    }
                    default:
                        s += e;
                        break;
                }
            } else {
                s += static_cast<char>(c);
                ++pos;
            }
        }
        error = "unterminated string";
        return false;
    }

    uint32_t ParseHex4() {
        uint32_t v = 0;
        for (int i = 0; i < 4 && pos < text.size(); ++i, ++pos) {
            char c = text[pos];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else return 0;
        }
        return v;
    }

    static void AppendUTF8(std::string& s, uint32_t cp) {
        if (cp < 0x80) {
            s += static_cast<char>(cp);
        } else if (cp < 0x800) {
            s += static_cast<char>(0xC0 | (cp >> 6));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s += static_cast<char>(0xE0 | (cp >> 12));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            s += static_cast<char>(0xF0 | (cp >> 18));
            s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool ParseBool(Json& out) {
        if (text.compare(pos, 4, "true") == 0) {
            pos += 4;
            out = Json();
            out.type_ = Json::Type::Bool;
            out.bool_ = true;
            return true;
        }
        if (text.compare(pos, 5, "false") == 0) {
            pos += 5;
            out = Json();
            out.type_ = Json::Type::Bool;
            out.bool_ = false;
            return true;
        }
        error = "invalid literal";
        return false;
    }

    bool ParseNull(Json& out) {
        if (text.compare(pos, 4, "null") == 0) {
            pos += 4;
            out = Json();
            return true;
        }
        error = "invalid literal";
        return false;
    }

    bool ParseNumber(Json& out) {
        size_t start = pos;
        if (pos < text.size() && text[pos] == '-') ++pos;
        while (pos < text.size() &&
               (std::isdigit(static_cast<unsigned char>(text[pos])) || text[pos] == '.' ||
                text[pos] == 'e' || text[pos] == 'E' || text[pos] == '+' || text[pos] == '-')) {
            ++pos;
        }
        out = Json();
        out.type_ = Json::Type::Number;
        out.number_ = std::strtod(text.c_str() + start, nullptr);
        return true;
    }
};

} // namespace

Json Json::Parse(const std::string& text, std::string* error) {
    Parser p(text);
    Json root;
    if (!p.ParseValue(root)) {
        if (error) *error = p.error;
        return {};
    }
    return root;
}

std::string JsonWriter::Escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string JsonWriter::Write(const Json& v) {
    std::string out;
    std::function<void(const Json&)> emit = [&](const Json& j) {
        switch (j.type()) {
            case Json::Type::Null:
                out += "null";
                break;
            case Json::Type::Bool:
                out += j.GetBool() ? "true" : "false";
                break;
            case Json::Type::Number: {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%g", j.GetNumber());
                out += buf;
                break;
            }
            case Json::Type::String:
                out += '"' + Escape(j.GetString()) + '"';
                break;
            case Json::Type::Array: {
                out += '[';
                for (size_t i = 0; i < j.Size(); ++i) {
                    if (i) out += ',';
                    emit(*j.At(i));
                }
                out += ']';
                break;
            }
            case Json::Type::Object: {
                out += '{';
                bool first = true;
                for (const auto& [key, val] : j.Members()) {
                    if (!first) out += ',';
                    first = false;
                    out += '"' + Escape(key) + "\":";
                    emit(val);
                }
                out += '}';
                break;
            }
        }
    };
    emit(v);
    return out;
}

bool JsonEquals(const Json& a, const Json& b) {
    if (a.type() != b.type()) return false;
    switch (a.type()) {
        case Json::Type::Null: return true;
        case Json::Type::Bool: return a.bool_ == b.bool_;
        case Json::Type::Number: return a.number_ == b.number_;
        case Json::Type::String: return a.string_ == b.string_;
        case Json::Type::Array: {
            if (a.array_.size() != b.array_.size()) return false;
            for (size_t i = 0; i < a.array_.size(); ++i)
                if (!JsonEquals(a.array_[i], b.array_[i])) return false;
            return true;
        }
        case Json::Type::Object: {
            if (a.object_.size() != b.object_.size()) return false;
            for (const auto& [k, v] : a.object_) {
                auto it = b.object_.find(k);
                if (it == b.object_.end() || !JsonEquals(v, it->second)) return false;
            }
            return true;
        }
    }
    return false;
}

} // namespace neon::core
