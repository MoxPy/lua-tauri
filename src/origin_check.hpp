#ifndef ORIGIN_CHECK_HPP
#define ORIGIN_CHECK_HPP

#include <string>
#include <algorithm>
#include <cctype>

class OriginValidator {
public:
    static bool is_allowed(const std::string &url, bool dev_mode = false, const std::string &dev_url = "") {
        if (url.empty()) return false;

        // Reject any userinfo '@' in URL before scheme/host parsing
        // (prevents http://localhost:80@evil.example bypasses)
        if (url.find('@') != std::string::npos) {
            return false;
        }

        // Normalize scheme and host to lowercase
        std::string norm_url = url;
        size_t scheme_end = norm_url.find("://");
        if (scheme_end != std::string::npos) {
            for (size_t i = 0; i < scheme_end; ++i) {
                norm_url[i] = tolower((unsigned char)norm_url[i]);
            }
            size_t host_start = scheme_end + 3;
            size_t host_end = norm_url.find_first_of("/?#:", host_start);
            if (host_end == std::string::npos) host_end = norm_url.length();
            for (size_t i = host_start; i < host_end; ++i) {
                norm_url[i] = tolower((unsigned char)norm_url[i]);
            }
        }

        // Production app origins allowed unconditionally:
        // tauri://localhost or https://tauri.localhost
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

        // In release mode, NO other origins (including localhost) are trusted for IPC.
        if (!dev_mode) {
            return false;
        }

        // In dev mode, ONLY the explicitly configured dev_url (or default http://localhost:1420)
        // is allowed, with strict scheme, host, and port matching.
        std::string target_dev = dev_url.empty() ? "http://localhost:1420" : dev_url;
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

        // If target_dev uses localhost, also allow 127.0.0.1 with identical port
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