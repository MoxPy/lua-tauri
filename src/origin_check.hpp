#ifndef ORIGIN_CHECK_HPP
#define ORIGIN_CHECK_HPP

#include <string>
#include <vector>

class OriginValidator {
public:
    static bool is_allowed(const std::string &url) {
        if (url.rfind("tauri://localhost", 0) == 0) {
            return true;
        }
        if (url.rfind("https://tauri.localhost", 0) == 0) {
            return true;
        }
        if (url.rfind("http://localhost", 0) == 0) {
            return true;
        }
        if (url.rfind("http://127.0.0.1", 0) == 0) {
            return true;
        }
        return false;
    }
};

#endif