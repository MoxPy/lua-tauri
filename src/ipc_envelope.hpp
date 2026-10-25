#pragma once
#include <string>
#include <cctype>
#include <cstdlib>

inline bool is_valid_seq_id(const std::string& seq) {
    if (seq.empty() || seq.size() > 64) return false;
    for (unsigned char c : seq) {
        if (!std::isalnum(c) && c != '_' && c != '-') return false;
    }
    return true;
}

inline bool constant_time_eq(const std::string &a, const std::string &b) {
    if (a.length() != b.length()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.length(); ++i) {
        diff |= (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]));
    }
    return diff == 0;
}

inline bool verify_envelope_token(const std::string &req, const std::string &expected_token) {
    if (expected_token.empty()) return false;
    size_t lbracket = req.find('[');
    if (lbracket == std::string::npos) return false;
    for (size_t i = 0; i < lbracket; ++i) {
        if (!std::isspace(static_cast<unsigned char>(req[i]))) return false;
    }
    size_t rbracket = req.rfind(']');
    if (rbracket == std::string::npos || rbracket <= lbracket) return false;
    for (size_t i = rbracket + 1; i < req.size(); ++i) {
        if (!std::isspace(static_cast<unsigned char>(req[i]))) return false;
    }
    size_t q_end = req.rfind('"', rbracket);
    if (q_end == std::string::npos || q_end <= lbracket) return false;
    size_t q_start = q_end;
    while (q_start > 0) {
        q_start--;
        if (req[q_start] == '"') {
            size_t bsl = 0;
            size_t b = q_start;
            while (b > 0 && req[b - 1] == '\\') { bsl++; b--; }
            if (bsl % 2 == 0) break;
        }
    }
    if (q_start >= q_end) return false;
    size_t comma = req.rfind(',', q_start);
    if (comma == std::string::npos || comma <= lbracket) return false;
    for (size_t i = comma + 1; i < q_start; ++i) {
        if (!std::isspace(static_cast<unsigned char>(req[i]))) return false;
    }
    for (size_t i = q_end + 1; i < rbracket; ++i) {
        if (!std::isspace(static_cast<unsigned char>(req[i]))) return false;
    }
    std::string token_in_req = req.substr(q_start + 1, q_end - (q_start + 1));
    return constant_time_eq(token_in_req, expected_token);
}

inline void append_utf8_codepoint(std::string &out, unsigned int cp) {
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

inline bool parse_json_string_literal(const std::string &str, size_t &pos, std::string &out) {
    if (pos >= str.size() || str[pos] != '"') return false;
    pos++;
    out.clear();
    while (pos < str.size()) {
        char c = str[pos];
        if (c == '"') {
            pos++;
            return true;
        } else if (c == '\\') {
            if (pos + 1 >= str.size()) return false;
            pos++;
            char esc = str[pos];
            switch (esc) {
                case '"':  out.push_back('"'); pos++; break;
                case '\\': out.push_back('\\'); pos++; break;
                case '/':  out.push_back('/'); pos++; break;
                case 'b':  out.push_back('\b'); pos++; break;
                case 'f':  out.push_back('\f'); pos++; break;
                case 'n':  out.push_back('\n'); pos++; break;
                case 'r':  out.push_back('\r'); pos++; break;
                case 't':  out.push_back('\t'); pos++; break;
                case 'u': {
                    if (pos + 4 >= str.size()) return false;
                    std::string hex = str.substr(pos + 1, 4);
                    for (unsigned char hc : hex) {
                        if (!std::isxdigit(hc)) return false;
                    }
                    unsigned int cp = static_cast<unsigned int>(std::strtoul(hex.c_str(), nullptr, 16));
                    pos += 5;
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos + 5 < str.size() && str[pos] == '\\' && str[pos + 1] == 'u') {
                        std::string low_hex = str.substr(pos + 2, 4);
                        bool ok_hex = true;
                        for (unsigned char hc : low_hex) {
                            if (!std::isxdigit(hc)) ok_hex = false;
                        }
                        if (ok_hex) {
                            unsigned int low = static_cast<unsigned int>(std::strtoul(low_hex.c_str(), nullptr, 16));
                            if (low >= 0xDC00 && low <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                                pos += 6;
                            }
                        }
                    }
                    append_utf8_codepoint(out, cp);
                    break;
                }
                default:
                    out.push_back(esc);
                    pos++;
                    break;
            }
        } else {
            out.push_back(c);
            pos++;
        }
    }
    return false;
}

inline std::string extract_json_string(const std::string &str, const std::string &key) {
    size_t pos = 0;
    int depth = 0;
    while (pos < str.size()) {
        while (pos < str.size() && std::isspace(static_cast<unsigned char>(str[pos]))) pos++;
        if (pos >= str.size()) break;
        char c = str[pos];
        if (c == '{') {
            depth++;
            pos++;
        } else if (c == '}') {
            depth--;
            pos++;
        } else if (c == '"') {
            std::string current_str;
            if (!parse_json_string_literal(str, pos, current_str)) return "";
            while (pos < str.size() && std::isspace(static_cast<unsigned char>(str[pos]))) pos++;
            if (depth == 1 && pos < str.size() && str[pos] == ':') {
                pos++;
                while (pos < str.size() && std::isspace(static_cast<unsigned char>(str[pos]))) pos++;
                if (current_str == key) {
                    std::string val;
                    if (parse_json_string_literal(str, pos, val)) {
                        return val;
                    }
                    return "";
                } else if (pos < str.size() && str[pos] == '"') {
                    std::string dummy;
                    if (!parse_json_string_literal(str, pos, dummy)) return "";
                }
            }
        } else {
            pos++;
        }
    }
    return "";
}
