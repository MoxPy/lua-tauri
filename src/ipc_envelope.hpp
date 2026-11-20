#pragma once
#include <string>
#include <cctype>
#include "json_parser.hpp"

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
    bool found_open = false;
    while (q_start > lbracket) {
        q_start--;
        if (req[q_start] == '"') {
            size_t bsl = 0;
            size_t b = q_start;
            while (b > 0 && req[b - 1] == '\\') { bsl++; b--; }
            if (bsl % 2 == 0) {
                found_open = true;
                break;
            }
        }
    }
    if (!found_open || q_start >= q_end) return false;
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

inline std::string extract_json_string(const std::string &str, const std::string &key) {
    JsonVal root;
    if (!parse_json_strict(str, root, 64)) return "";
    if (!root.is_object() || !root.has(key)) return "";
    const JsonVal &v = root[key];
    return v.is_string() ? v.str() : "";
}
