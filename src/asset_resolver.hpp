#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <sstream>
#include <cctype>
#include <algorithm>
#include <cstdio>
#include "json_parser.hpp"

inline std::string get_mime_type(const std::string &path) {
    auto ends_with = [](const std::string &s, const std::string &suffix) {
        if (s.size() < suffix.size()) return false;
        return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    std::string lower = path;
    for (char &c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (ends_with(lower, ".html") || ends_with(lower, ".htm")) return "text/html";
    if (ends_with(lower, ".js") || ends_with(lower, ".mjs") || ends_with(lower, ".cjs")) return "application/javascript";
    if (ends_with(lower, ".css")) return "text/css";
    if (ends_with(lower, ".json") || ends_with(lower, ".map")) return "application/json";
    if (ends_with(lower, ".png")) return "image/png";
    if (ends_with(lower, ".jpg") || ends_with(lower, ".jpeg")) return "image/jpeg";
    if (ends_with(lower, ".gif")) return "image/gif";
    if (ends_with(lower, ".webp")) return "image/webp";
    if (ends_with(lower, ".avif")) return "image/avif";
    if (ends_with(lower, ".svg")) return "image/svg+xml";
    if (ends_with(lower, ".ico")) return "image/x-icon";
    if (ends_with(lower, ".woff2")) return "font/woff2";
    if (ends_with(lower, ".woff")) return "font/woff";
    if (ends_with(lower, ".ttf")) return "font/ttf";
    if (ends_with(lower, ".otf")) return "font/otf";
    if (ends_with(lower, ".wasm")) return "application/wasm";
    if (ends_with(lower, ".mp3")) return "audio/mpeg";
    if (ends_with(lower, ".ogg")) return "audio/ogg";
    if (ends_with(lower, ".wav")) return "audio/wav";
    if (ends_with(lower, ".mp4")) return "video/mp4";
    if (ends_with(lower, ".webm")) return "video/webm";
    if (ends_with(lower, ".pdf")) return "application/pdf";
    if (ends_with(lower, ".xml")) return "application/xml";
    if (ends_with(lower, ".txt")) return "text/plain";
    return "application/octet-stream";
}

inline bool resolve_contained_asset(const std::string &assets_root, const std::string &raw_path, std::string &out_real_path) {
    if (assets_root.empty() || raw_path.empty()) return false;
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

    std::error_code ec;
    std::filesystem::path root_p(assets_root);
    std::filesystem::path canonical_root = std::filesystem::canonical(root_p, ec);
    if (ec) {
        ec.clear();
        canonical_root = std::filesystem::weakly_canonical(root_p, ec);
        if (ec) return false;
    }

    std::filesystem::path target = canonical_root;
    for (const auto &p : parts) {
        target /= p;
    }

    std::filesystem::path canonical_target = std::filesystem::canonical(target, ec);
    if (ec) return false;

    auto root_str = canonical_root.generic_string();
    auto target_str = canonical_target.generic_string();
    while (root_str.size() > 1 && root_str.back() == '/') {
        root_str.pop_back();
    }
    if (target_str.size() < root_str.size() || target_str.compare(0, root_str.size(), root_str) != 0) {
        return false;
    }
    if (root_str != "/" && target_str.size() > root_str.size() && target_str[root_str.size()] != '/') {
        return false;
    }

    if (!std::filesystem::is_regular_file(canonical_target, ec) || ec) {
        return false;
    }

    out_real_path = canonical_target.string();
    return true;
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

namespace html_scan_detail {

inline bool iequals_at(const std::string &s, size_t pos, const char *lit, size_t len) {
    if (pos + len > s.size()) return false;
    for (size_t i = 0; i < len; ++i) {
        if (std::tolower(static_cast<unsigned char>(s[pos + i])) != static_cast<unsigned char>(lit[i])) {
            return false;
        }
    }
    return true;
}

inline size_t find_tag_end(const std::string &html, size_t start) {
    char quote = 0;
    for (size_t i = start; i < html.size(); ++i) {
        char c = html[i];
        if (quote != 0) {
            if (c == quote) quote = 0;
        } else {
            if (c == '"' || c == '\'') quote = c;
            else if (c == '>') return i;
        }
    }
    return std::string::npos;
}

inline size_t skip_raw_text_element(const std::string &html, size_t after_open, const char *close_tag, size_t close_len) {
    size_t pos = after_open;
    while (pos < html.size()) {
        size_t lt = html.find('<', pos);
        if (lt == std::string::npos) return html.size();
        if (iequals_at(html, lt, close_tag, close_len)) {
            size_t end = find_tag_end(html, lt + close_len);
            return (end == std::string::npos) ? html.size() : (end + 1);
        }
        pos = lt + 1;
    }
    return html.size();
}

inline bool tag_has_csp_http_equiv(const std::string &tag) {
    size_t pos = 0;
    while (pos < tag.size() && !std::isspace(static_cast<unsigned char>(tag[pos])) && tag[pos] != '>') pos++;
    while (pos < tag.size()) {
        while (pos < tag.size() && (std::isspace(static_cast<unsigned char>(tag[pos])) || tag[pos] == '/')) pos++;
        if (pos >= tag.size() || tag[pos] == '>') break;
        size_t name_start = pos;
        while (pos < tag.size() && !std::isspace(static_cast<unsigned char>(tag[pos])) && tag[pos] != '=' && tag[pos] != '>') pos++;
        std::string attr_name;
        for (size_t i = name_start; i < pos; ++i) {
            attr_name.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(tag[i]))));
        }
        while (pos < tag.size() && std::isspace(static_cast<unsigned char>(tag[pos]))) pos++;
        std::string attr_val;
        if (pos < tag.size() && tag[pos] == '=') {
            pos++;
            while (pos < tag.size() && std::isspace(static_cast<unsigned char>(tag[pos]))) pos++;
            if (pos < tag.size() && (tag[pos] == '"' || tag[pos] == '\'')) {
                char q = tag[pos++];
                size_t val_start = pos;
                while (pos < tag.size() && tag[pos] != q) pos++;
                for (size_t i = val_start; i < pos; ++i) {
                    attr_val.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(tag[i]))));
                }
                if (pos < tag.size()) pos++;
            } else {
                size_t val_start = pos;
                while (pos < tag.size() && !std::isspace(static_cast<unsigned char>(tag[pos])) && tag[pos] != '>') pos++;
                for (size_t i = val_start; i < pos; ++i) {
                    attr_val.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(tag[i]))));
                }
            }
        }
        if (attr_name == "http-equiv" && attr_val == "content-security-policy") {
            return true;
        }
    }
    return false;
}

}

inline bool has_existing_csp_meta(const std::string &html) {
    size_t pos = 0;
    while (pos < html.size()) {
        size_t lt = html.find('<', pos);
        if (lt == std::string::npos) break;
        if (html.compare(lt, 4, "<!--") == 0) {
            size_t end_c = html.find("-->", lt + 4);
            if (end_c == std::string::npos) break;
            pos = end_c + 3;
            continue;
        }
        if (html_scan_detail::iequals_at(html, lt, "<script", 7) &&
            (lt + 7 < html.size() && (std::isspace(static_cast<unsigned char>(html[lt + 7])) || html[lt + 7] == '>'))) {
            size_t tag_end = html_scan_detail::find_tag_end(html, lt + 7);
            if (tag_end == std::string::npos) break;
            pos = html_scan_detail::skip_raw_text_element(html, tag_end + 1, "</script", 8);
            continue;
        }
        if (html_scan_detail::iequals_at(html, lt, "<style", 6) &&
            (lt + 6 < html.size() && (std::isspace(static_cast<unsigned char>(html[lt + 6])) || html[lt + 6] == '>'))) {
            size_t tag_end = html_scan_detail::find_tag_end(html, lt + 6);
            if (tag_end == std::string::npos) break;
            pos = html_scan_detail::skip_raw_text_element(html, tag_end + 1, "</style", 7);
            continue;
        }
        if (html_scan_detail::iequals_at(html, lt, "<textarea", 9) &&
            (lt + 9 < html.size() && (std::isspace(static_cast<unsigned char>(html[lt + 9])) || html[lt + 9] == '>'))) {
            size_t tag_end = html_scan_detail::find_tag_end(html, lt + 9);
            if (tag_end == std::string::npos) break;
            pos = html_scan_detail::skip_raw_text_element(html, tag_end + 1, "</textarea", 10);
            continue;
        }
        if (html_scan_detail::iequals_at(html, lt, "<meta", 5) &&
            (lt + 5 < html.size() && std::isspace(static_cast<unsigned char>(html[lt + 5])))) {
            size_t tag_end = html_scan_detail::find_tag_end(html, lt + 5);
            if (tag_end == std::string::npos) break;
            std::string tag = html.substr(lt, tag_end - lt + 1);
            if (html_scan_detail::tag_has_csp_http_equiv(tag)) {
                return true;
            }
            pos = tag_end + 1;
            continue;
        }
        pos = lt + 1;
    }
    return false;
}

inline void inject_csp_meta(std::string &html, const std::string &csp) {
    if (csp.empty() || has_existing_csp_meta(html)) return;
    std::string safe_csp = escape_html_attr(csp);
    std::string meta_tag = "\n  <meta http-equiv=\"Content-Security-Policy\" content=\"" + safe_csp + "\">";
    size_t head_insert = std::string::npos;
    size_t html_insert = std::string::npos;
    size_t pos = 0;
    while (pos < html.size()) {
        size_t lt = html.find('<', pos);
        if (lt == std::string::npos) break;
        if (html.compare(lt, 4, "<!--") == 0) {
            size_t end_c = html.find("-->", lt + 4);
            if (end_c == std::string::npos) break;
            pos = end_c + 3;
            continue;
        }
        if (html_scan_detail::iequals_at(html, lt, "<head", 5) &&
            (lt + 5 < html.size() && (std::isspace(static_cast<unsigned char>(html[lt + 5])) || html[lt + 5] == '>'))) {
            size_t tag_end = html_scan_detail::find_tag_end(html, lt + 5);
            if (tag_end != std::string::npos) {
                head_insert = tag_end + 1;
                break;
            }
        }
        if (html_insert == std::string::npos && html_scan_detail::iequals_at(html, lt, "<html", 5) &&
            (lt + 5 < html.size() && (std::isspace(static_cast<unsigned char>(html[lt + 5])) || html[lt + 5] == '>'))) {
            size_t tag_end = html_scan_detail::find_tag_end(html, lt + 5);
            if (tag_end != std::string::npos) {
                html_insert = tag_end + 1;
            }
        }
        pos = lt + 1;
    }
    if (head_insert != std::string::npos) {
        html.insert(head_insert, meta_tag);
    } else if (html_insert != std::string::npos) {
        html.insert(html_insert, "<head>" + meta_tag + "\n</head>");
    } else {
        html = "<head>" + meta_tag + "\n</head>\n" + html;
    }
}
