#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <sstream>
#include <cctype>
#include <algorithm>
#include <regex>
#include <cstdio>

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

inline bool resolve_contained_asset(const std::string &assets_root, const std::string &raw_path, std::string &out_real_path) {
    if (raw_path.empty()) return false;
    if (raw_path.find('\0') != std::string::npos) return false;

    size_t q_or_h = raw_path.find_first_of("?#");
    std::string clean_raw = (q_or_h == std::string::npos) ? raw_path : raw_path.substr(0, q_or_h);
    if (clean_raw.empty()) clean_raw = "/";

    std::string path;
    for (size_t i = 0; i < clean_raw.size(); ++i) {
        if (clean_raw[i] == '%' && i + 2 < clean_raw.size() &&
            std::isxdigit(static_cast<unsigned char>(clean_raw[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(clean_raw[i + 2]))) {
            char hex[3] = { clean_raw[i + 1], clean_raw[i + 2], '\0' };
            int val = static_cast<int>(std::strtol(hex, nullptr, 16));
            if (val == 0) return false;
            path += static_cast<char>(val);
            i += 2;
        } else {
            path += clean_raw[i];
        }
    }

    if (path.find('\0') != std::string::npos) return false;
    if (path.find('\\') != std::string::npos || path.find(':') != std::string::npos) return false;

    if (path.empty() || path == "/") path = "/index.html";

    std::vector<std::string> parts;
    std::stringstream ss(path);
    std::string part;
    while (std::getline(ss, part, '/')) {
        if (part.empty() || part == ".") continue;
        if (part == "..") return false;
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

inline std::string escape_html_attr(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c == '\r' || c == '\n' || c == '\0') continue;
        switch (c) {
            case '"':  out += "&quot;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '&':  out += "&amp;"; break;
            default:   out.push_back(static_cast<char>(c)); break;
        }
    }
    return out;
}

inline std::string strip_html_comments(const std::string &html) {
    std::string out;
    out.reserve(html.size());
    size_t pos = 0;
    while (pos < html.size()) {
        size_t start = html.find("<!--", pos);
        if (start == std::string::npos) {
            out.append(html, pos, std::string::npos);
            break;
        }
        out.append(html, pos, start - pos);
        size_t end = html.find("-->", start + 4);
        if (end == std::string::npos) {
            break;
        }
        pos = end + 3;
    }
    return out;
}

inline bool has_existing_csp_meta(const std::string &html) {
    std::string uncommented = strip_html_comments(html);
    try {
        static const std::regex csp_meta_re(
            "<meta\\s+[^>]*http-equiv\\s*=\\s*[\"']?content-security-policy[\"']?[^>]*>",
            std::regex_constants::icase | std::regex_constants::optimize
        );
        return std::regex_search(uncommented, csp_meta_re);
    } catch (...) {
        return false;
    }
}

inline void inject_csp_meta(std::string &html, const std::string &csp) {
    if (csp.empty() || has_existing_csp_meta(html)) return;
    std::string safe_csp = escape_html_attr(csp);
    std::string meta_tag = "\n  <meta http-equiv=\"Content-Security-Policy\" content=\"" + safe_csp + "\">";
    try {
        static const std::regex head_re("<head(\\s+[^>]*)?>", std::regex_constants::icase | std::regex_constants::optimize);
        std::smatch m;
        if (std::regex_search(html, m, head_re)) {
            size_t insert_pos = static_cast<size_t>(m.position(0) + m.length(0));
            html.insert(insert_pos, meta_tag);
            return;
        }
        static const std::regex html_re("<html(\\s+[^>]*)?>", std::regex_constants::icase | std::regex_constants::optimize);
        if (std::regex_search(html, m, html_re)) {
            size_t insert_pos = static_cast<size_t>(m.position(0) + m.length(0));
            html.insert(insert_pos, "<head>" + meta_tag + "\n</head>");
            return;
        }
    } catch (...) {}
    html = "<head>" + meta_tag + "\n</head>\n" + html;
}
