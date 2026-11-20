#pragma once

#include <string>
#include <vector>
#include <map>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <cstdio>

struct JsonVal {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0.0;
    std::string s;
    std::vector<JsonVal> arr;
    std::map<std::string, JsonVal> obj;

    bool is_null() const { return type == Null; }
    bool is_bool() const { return type == Bool; }
    bool is_number() const { return type == Number; }
    bool is_string() const { return type == String; }
    bool is_array() const { return type == Array; }
    bool is_object() const { return type == Object; }

    bool has(const std::string &k) const {
        return type == Object && obj.find(k) != obj.end();
    }

    const JsonVal &operator[](const std::string &k) const {
        static const JsonVal empty_val;
        if (type != Object) return empty_val;
        auto it = obj.find(k);
        return it != obj.end() ? it->second : empty_val;
    }

    std::string str() const { return type == String ? s : ""; }
    bool boolean() const { return type == Bool ? b : false; }
    double num() const { return type == Number ? n : 0.0; }
};

inline std::string json_escape_string(const std::string &input) {
    std::string out = "\"";
    out.reserve(input.size() + 2);
    for (unsigned char c : input) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char hexbuf[8];
                    std::snprintf(hexbuf, sizeof(hexbuf), "\\u%04x", c);
                    out += hexbuf;
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    out += "\"";
    return out;
}

namespace json_detail {

inline void skip_ws(const std::string &s, size_t &pos) {
    while (pos < s.size()) {
        char c = s[pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            pos++;
        } else {
            break;
        }
    }
}

inline void append_utf8(std::string &out, unsigned int cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

inline bool parse_hex4(const std::string &s, size_t pos, unsigned int &out) {
    if (pos + 4 > s.size()) return false;
    unsigned int val = 0;
    for (size_t i = 0; i < 4; ++i) {
        unsigned char c = static_cast<unsigned char>(s[pos + i]);
        val <<= 4;
        if (c >= '0' && c <= '9') val |= (c - '0');
        else if (c >= 'a' && c <= 'f') val |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') val |= (c - 'A' + 10);
        else return false;
    }
    out = val;
    return true;
}

inline bool parse_string(const std::string &s, size_t &pos, std::string &out) {
    if (pos >= s.size() || s[pos] != '"') return false;
    pos++;
    out.clear();
    while (pos < s.size()) {
        unsigned char uc = static_cast<unsigned char>(s[pos]);
        if (uc == '"') {
            pos++;
            return true;
        }
        if (uc < 0x20) {
            return false;
        }
        if (uc == '\\') {
            pos++;
            if (pos >= s.size()) return false;
            char esc = s[pos++];
            switch (esc) {
                case '"':  out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/'); break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    unsigned int cp = 0;
                    if (!parse_hex4(s, pos, cp)) return false;
                    pos += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (pos + 6 > s.size() || s[pos] != '\\' || s[pos + 1] != 'u') return false;
                        unsigned int low = 0;
                        if (!parse_hex4(s, pos + 2, low)) return false;
                        if (low < 0xDC00 || low > 0xDFFF) return false;
                        pos += 6;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return false;
                    }
                    if (cp == 0) return false;
                    append_utf8(out, cp);
                    break;
                }
                default:
                    return false;
            }
        } else {
            out.push_back(static_cast<char>(uc));
            pos++;
        }
    }
    return false;
}

inline bool parse_number(const std::string &s, size_t &pos, double &out) {
    size_t start = pos;
    if (pos < s.size() && s[pos] == '-') pos++;
    if (pos >= s.size()) return false;
    if (s[pos] == '0') {
        pos++;
        if (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') return false;
    } else if (s[pos] >= '1' && s[pos] <= '9') {
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') pos++;
    } else {
        return false;
    }
    if (pos < s.size() && s[pos] == '.') {
        pos++;
        if (pos >= s.size() || s[pos] < '0' || s[pos] > '9') return false;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') pos++;
    }
    if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
        pos++;
        if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) pos++;
        if (pos >= s.size() || s[pos] < '0' || s[pos] > '9') return false;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') pos++;
    }
    std::string num_str = s.substr(start, pos - start);
    char *endptr = nullptr;
    double val = std::strtod(num_str.c_str(), &endptr);
    if (!endptr || endptr != num_str.c_str() + num_str.size() || !std::isfinite(val)) {
        return false;
    }
    out = val;
    return true;
}

inline bool parse_value(const std::string &s, size_t &pos, JsonVal &out, int depth, int max_depth) {
    if (depth > max_depth) return false;
    skip_ws(s, pos);
    if (pos >= s.size()) return false;
    char c = s[pos];
    if (c == '"') {
        out.type = JsonVal::String;
        return parse_string(s, pos, out.s);
    }
    if (c == '{') {
        out.type = JsonVal::Object;
        pos++;
        skip_ws(s, pos);
        if (pos < s.size() && s[pos] == '}') {
            pos++;
            return true;
        }
        while (pos < s.size()) {
            skip_ws(s, pos);
            std::string key;
            if (!parse_string(s, pos, key)) return false;
            skip_ws(s, pos);
            if (pos >= s.size() || s[pos] != ':') return false;
            pos++;
            JsonVal child;
            if (!parse_value(s, pos, child, depth + 1, max_depth)) return false;
            out.obj[std::move(key)] = std::move(child);
            skip_ws(s, pos);
            if (pos >= s.size()) return false;
            if (s[pos] == '}') {
                pos++;
                return true;
            }
            if (s[pos] != ',') return false;
            pos++;
        }
        return false;
    }
    if (c == '[') {
        out.type = JsonVal::Array;
        pos++;
        skip_ws(s, pos);
        if (pos < s.size() && s[pos] == ']') {
            pos++;
            return true;
        }
        while (pos < s.size()) {
            JsonVal child;
            if (!parse_value(s, pos, child, depth + 1, max_depth)) return false;
            out.arr.push_back(std::move(child));
            skip_ws(s, pos);
            if (pos >= s.size()) return false;
            if (s[pos] == ']') {
                pos++;
                return true;
            }
            if (s[pos] != ',') return false;
            pos++;
        }
        return false;
    }
    if (s.compare(pos, 4, "true") == 0) {
        out.type = JsonVal::Bool;
        out.b = true;
        pos += 4;
        return true;
    }
    if (s.compare(pos, 5, "false") == 0) {
        out.type = JsonVal::Bool;
        out.b = false;
        pos += 5;
        return true;
    }
    if (s.compare(pos, 4, "null") == 0) {
        out.type = JsonVal::Null;
        pos += 4;
        return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        out.type = JsonVal::Number;
        return parse_number(s, pos, out.n);
    }
    return false;
}

}

inline bool parse_json_strict(const std::string &input, JsonVal &out, int max_depth = 64) {
    out = JsonVal{};
    size_t pos = 0;
    if (!json_detail::parse_value(input, pos, out, 0, max_depth)) {
        out = JsonVal{};
        return false;
    }
    json_detail::skip_ws(input, pos);
    if (pos != input.size()) {
        out = JsonVal{};
        return false;
    }
    return true;
}
