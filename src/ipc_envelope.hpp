#pragma once

#include <string>
#include <cctype>

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
    size_t rbracket = req.rfind(']');
    if (rbracket == std::string::npos) return false;
    size_t q_end = req.rfind('"', rbracket);
    if (q_end == std::string::npos || q_end == 0) return false;
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
    if (comma == std::string::npos) return false;
    for (size_t i = comma + 1; i < q_start; ++i) {
        if (!isspace(static_cast<unsigned char>(req[i]))) return false;
    }
    for (size_t i = q_end + 1; i < rbracket; ++i) {
        if (!isspace(static_cast<unsigned char>(req[i]))) return false;
    }
    std::string token_in_req = req.substr(q_start + 1, q_end - (q_start + 1));
    return constant_time_eq(token_in_req, expected_token);
}

inline std::string extract_json_string(const std::string &str, const std::string &key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = str.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.length();
    while (pos < str.size() && (str[pos] == ' ' || str[pos] == ':')) pos++;
    if (pos >= str.size() || str[pos] != '"') return "";
    pos++; // skip opening quote
    std::string val;
    while (pos < str.size()) {
        char c = str[pos];
        if (c == '"') {
            return val;
        } else if (c == '\\' && pos + 1 < str.size()) {
            pos++;
            char esc = str[pos];
            if (esc == '"') val += '"';
            else if (esc == '\\') val += '\\';
            else if (esc == '/') val += '/';
            else if (esc == 'n') val += '\n';
            else if (esc == 'r') val += '\r';
            else if (esc == 't') val += '\t';
            else val += esc;
        } else {
            val += c;
        }
        pos++;
    }
    return "";
}
