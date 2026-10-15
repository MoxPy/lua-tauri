#ifndef ORIGIN_CHECK_HPP
#define ORIGIN_CHECK_HPP

#include <string>
#include <vector>

class OriginValidator {
public:
    static bool is_allowed(const std::string &url) {
        auto check_prefix = [](const std::string &str, const std::string &prefix) {
            if (str.rfind(prefix, 0) == 0) {
                if (str.length() == prefix.length()) return true;
                char next = str[prefix.length()];
                return next == ':' || next == '/' || next == '?';
            }
            return false;
        };
        if (check_prefix(url, "tauri://localhost")) return true;
        if (check_prefix(url, "https://tauri.localhost")) return true;
        if (check_prefix(url, "http://localhost")) return true;
        if (check_prefix(url, "http://127.0.0.1")) return true;
        return false;
    }
};

#endif