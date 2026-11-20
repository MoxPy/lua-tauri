#ifndef ORIGIN_CHECK_HPP
#define ORIGIN_CHECK_HPP

#include <string>
#include <algorithm>
#include <cctype>

class OriginValidator {
public:
    static bool has_authority_userinfo(const std::string &url) {
        size_t scheme_end = url.find("://");
        if (scheme_end == std::string::npos) return true;
        size_t auth_start = scheme_end + 3;
        size_t auth_end = url.find_first_of("/?#", auth_start);
        if (auth_end == std::string::npos) auth_end = url.size();
        for (size_t i = auth_start; i < auth_end; ++i) {
            if (url[i] == '@' || url[i] == '\\') return true;
        }
        return false;
    }

    static std::string normalize_origin_url(const std::string &raw) {
        std::string norm = raw;
        size_t scheme_end = norm.find("://");
        if (scheme_end != std::string::npos) {
            for (size_t i = 0; i < scheme_end; ++i) {
                norm[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(norm[i])));
            }
            size_t host_start = scheme_end + 3;
            size_t host_end = norm.find_first_of("/?#:", host_start);
            if (host_start < norm.size() && norm[host_start] == '[') {
                size_t close_br = norm.find(']', host_start);
                if (close_br != std::string::npos) host_end = close_br + 1;
            }
            if (host_end == std::string::npos) host_end = norm.length();
            for (size_t i = host_start; i < host_end; ++i) {
                norm[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(norm[i])));
            }
        }
        return norm;
    }

    static bool is_allowed(const std::string &url, bool dev_mode = false, const std::string &dev_url = "") {
        if (url.empty()) return false;
        if (has_authority_userinfo(url)) {
            return false;
        }

        std::string norm_url = normalize_origin_url(url);

        if (norm_url == "tauri://localhost" || norm_url == "tauri://localhost/" ||
            norm_url.rfind("tauri://localhost/", 0) == 0 ||
            norm_url.rfind("tauri://localhost?", 0) == 0 ||
            norm_url.rfind("tauri://localhost#", 0) == 0) {
            return true;
        }

        if (norm_url == "https://tauri.localhost" || norm_url == "https://tauri.localhost/" ||
            norm_url.rfind("https://tauri.localhost/", 0) == 0 ||
            norm_url.rfind("https://tauri.localhost?", 0) == 0 ||
            norm_url.rfind("https://tauri.localhost#", 0) == 0) {
            return true;
        }

        if (!dev_mode) {
            return false;
        }

        std::string target_dev = dev_url.empty() ? "http://localhost:1420" : normalize_origin_url(dev_url);
        if (has_authority_userinfo(target_dev)) {
            return false;
        }
        while (!target_dev.empty() && target_dev.back() == '/') {
            target_dev.pop_back();
        }

        auto check_origin_prefix = [](const std::string &u, const std::string &expected) {
            if (u.rfind(expected, 0) == 0) {
                if (u.length() == expected.length()) return true;
                char next = u[expected.length()];
                return next == '/' || next == '?' || next == '#';
            }
            return false;
        };

        if (check_origin_prefix(norm_url, target_dev)) {
            return true;
        }

        size_t lh = target_dev.find("://localhost");
        if (lh != std::string::npos) {
            size_t after_lh = lh + 12;
            if (after_lh == target_dev.size() || target_dev[after_lh] == ':' || target_dev[after_lh] == '/') {
                std::string suffix = target_dev.substr(after_lh);
                std::string scheme_prefix = target_dev.substr(0, lh);
                if (check_origin_prefix(norm_url, scheme_prefix + "://127.0.0.1" + suffix)) {
                    return true;
                }
                if (check_origin_prefix(norm_url, scheme_prefix + "://[::1]" + suffix)) {
                    return true;
                }
            }
        }

        return false;
    }
};

#endif
