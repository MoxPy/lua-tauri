#pragma once

#include "origin_check.hpp"
#include <string>
#include <algorithm>

namespace SecurityPolicy {

inline std::string build_default_csp(bool dev_mode, const std::string &custom_csp = "") {
    if (!custom_csp.empty()) {
        return custom_csp;
    }
    if (dev_mode) {
        return "default-src 'self' tauri: asset:; "
               "script-src 'self' tauri: asset: 'unsafe-inline'; "
               "style-src 'self' 'unsafe-inline'; "
               "img-src 'self' asset: data: tauri:; "
               "connect-src 'self' tauri: ipc: https://tauri.localhost http://localhost:* ws://localhost:* http://127.0.0.1:* ws://127.0.0.1:*; "
               "frame-src 'none'; "
               "object-src 'none'; "
               "base-uri 'self'; "
               "form-action 'self'; "
               "frame-ancestors 'none';";
    }
    return "default-src 'self' tauri: asset:; "
           "script-src 'self' tauri: asset:; "
           "style-src 'self' 'unsafe-inline'; "
           "img-src 'self' asset: data: tauri:; "
           "connect-src 'self' tauri: ipc: https://tauri.localhost; "
           "frame-src 'none'; "
           "object-src 'none'; "
           "base-uri 'self'; "
           "form-action 'self'; "
           "frame-ancestors 'none';";
}

inline bool is_navigation_allowed(const std::string &target_url, bool dev_mode = false, const std::string &dev_url = "") {
    if (target_url.empty()) return false;
    // Allow about:blank for internal webview resets
    if (target_url == "about:blank") return true;
    return OriginValidator::is_allowed(target_url, dev_mode, dev_url);
}

inline bool is_frame_source_allowed(const std::string &source_url, const std::string &top_url, bool dev_mode = false, const std::string &dev_url = "") {
    if (source_url.empty()) return false;
    if (!OriginValidator::is_allowed(source_url, dev_mode, dev_url)) return false;
    if (!top_url.empty() && !OriginValidator::is_allowed(top_url, dev_mode, dev_url)) return false;
    return true;
}

} // namespace SecurityPolicy
