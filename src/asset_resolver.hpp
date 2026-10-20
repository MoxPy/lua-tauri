#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <sstream>
#include <cctype>
#include <algorithm>
#include <regex>
#include <cstdio>

// ---------------------------------------------------------------------------
// MIME Type Resolution
// ---------------------------------------------------------------------------
inline std::string get_mime_type(const std::string &path) {
    auto ends_with = [](const std::string &s, const std::string &suffix) {
        if (s.size() < suffix.size()) return false;
        return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (ends_with(lower, ".html") || ends_with(lower, ".htm")) return "text/html";
    if (ends_with(lower, ".js") || ends_with(lower, ".mjs")) return "application/javascript";
    if (ends_with(lower, ".css")) return "text/css";
    if (ends_with(lower, ".json")) return "application/json";
    if (ends_with(lower, ".png")) return "image/png";
    if (ends_with(lower, ".jpg") || ends_with(lower, ".jpeg")) return "image/jpeg";
    if (ends_with(lower, ".svg")) return "image/svg+xml";
    if (ends_with(lower, ".wasm")) return "application/wasm";
    if (ends_with(lower, ".ico")) return "image/x-icon";
    if (ends_with(lower, ".txt")) return "text/plain";
    return "application/octet-stream";
}

// ---------------------------------------------------------------------------
// Contained Asset Path Resolution & Symlink Jail Escape Defense
// ---------------------------------------------------------------------------
inline bool resolve_contained_asset(const std::string &assets_root, const std::string &raw_path, std::string &out_real_path) {
    if (raw_path.empty()) return false;
    // Decode percent-encoded characters
    std::string path;
    for (size_t i = 0; i < raw_path.size(); ++i) {
        if (raw_path[i] == '%' && i + 2 < raw_path.size() && isxdigit((unsigned char)raw_path[i+1]) && isxdigit((unsigned char)raw_path[i+2])) {
            char hex[3] = { raw_path[i+1], raw_path[i+2], '\0' };
            int val = (int)strtol(hex, nullptr, 16);
            if (val == 0) return false; // NUL byte injection rejected
            path += (char)val;
            i += 2;
        } else {
            path += raw_path[i];
        }
    }
    if (path.find('\0') != std::string::npos) return false;
    if (path.find('\\') != std::string::npos || path.find(':') != std::string::npos) return false;

    // Default to /index.html if empty or root
    if (path.empty() || path == "/") path = "/index.html";

    // Split components and verify no traversal
    std::vector<std::string> parts;
    std::stringstream ss(path);
    std::string part;
    while (std::getline(ss, part, '/')) {
        if (part.empty() || part == ".") continue;
        if (part == "..") return false; // Traversal attempt rejected
        parts.push_back(part);
    }
    if (parts.empty()) parts.push_back("index.html");

    std::filesystem::path root_p(assets_root);
    std::error_code ec;
    std::filesystem::path canonical_root = std::filesystem::weakly_canonical(root_p, ec);
    if (ec) return false;

    std::filesystem::path target = canonical_root;
    for (const auto &p : parts) {
        target /= p;
    }

    std::filesystem::path canonical_target = std::filesystem::weakly_canonical(target, ec);
    if (ec) return false;

    // Verify canonical target is within canonical root
    auto root_str = canonical_root.string();
    auto target_str = canonical_target.string();
    if (target_str.size() < root_str.size() || target_str.compare(0, root_str.size(), root_str) != 0) {
        return false;
    }
    if (target_str.size() > root_str.size() && target_str[root_str.size()] != '/' && target_str[root_str.size()] != '\\') {
        return false;
    }

    if (!std::filesystem::exists(canonical_target, ec) || std::filesystem::is_directory(canonical_target, ec)) {
        return false;
    }

    out_real_path = canonical_target.string();
    return true;
}

// ---------------------------------------------------------------------------
// RFC 8259 Strict JSON String Serialization
// ---------------------------------------------------------------------------
inline std::string json_escape_string(const std::string &input) {
    std::string out = "\"";
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
                    snprintf(hexbuf, sizeof(hexbuf), "\\u%04x", c);
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

// ---------------------------------------------------------------------------
// Robust Content-Security-Policy (CSP) Injection
// ---------------------------------------------------------------------------
inline bool has_existing_csp_meta(const std::string &html) {
    std::regex csp_regex(R"(<meta\s+[^>]*http-equiv\s*=\s*["']?content-security-policy["']?[^>]*>)", std::regex::icase);
    return std::regex_search(html, csp_regex);
}

inline void inject_csp_meta(std::string &html, const std::string &csp) {
    if (csp.empty() || has_existing_csp_meta(html)) {
        return;
    }
    std::string meta_tag = "\n  <meta http-equiv=\"Content-Security-Policy\" content=\"" + csp + "\">";

    std::regex head_regex(R"(<head(\s+[^>]*)?>)", std::regex::icase);
    std::smatch m;
    if (std::regex_search(html, m, head_regex)) {
        html.insert(m.position() + m.length(), meta_tag);
        return;
    }

    std::regex html_regex(R"(<html(\s+[^>]*)?>)", std::regex::icase);
    if (std::regex_search(html, m, html_regex)) {
        std::string head_block = "\n<head>" + meta_tag + "\n</head>";
        html.insert(m.position() + m.length(), head_block);
        return;
    }

    html = "<meta http-equiv=\"Content-Security-Policy\" content=\"" + csp + "\">\n" + html;
}
