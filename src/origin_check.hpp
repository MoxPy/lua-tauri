#ifndef ORIGIN_CHECK_HPP
#define ORIGIN_CHECK_HPP

#include <string>
#include <algorithm>
#include <cctype>

class OriginValidator {
public:
    static std::string normalize_origin_url(const std::string &raw) {
        std::string norm = raw;
        size_t scheme_end = norm.find("://");
        if (scheme_end != std::string::npos) {
            for (size_t i = 0; i < scheme_end; ++i) {
                norm[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(norm[i])));
            }
            size_t host_start = scheme_end + 3;
            size_t host_end = norm.find_first_of("/?#:", host_start);
            if (host_end == std::string::npos) host_end = norm.length();
            for (size_t i = host_start; i < host_end; ++i) {
                norm[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(norm[i])));
            }
        }
        return norm;
    }

    static bool is_allowed(const std::string &url, bool dev_mode = false, const std::string &dev_url = "") {
        if (url.empty()) return false;
        if (url.find('@') != std::string::npos) {
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
        if (target_dev.find('@') != std::string::npos) {
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
            std::string ip_dev = target_dev.substr(0, lh) + "://127.0.0.1" + target_dev.substr(lh + 12);
            if (check_origin_prefix(norm_url, ip_dev)) {
                return true;
            }
        }

        return false;
    }
};

#endif