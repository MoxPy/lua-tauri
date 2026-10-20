#ifndef RESOURCE_RESOLVER_HPP
#define RESOURCE_RESOLVER_HPP

#include <string>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <filesystem>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace ResourceResolver {

inline std::string get_executable_dir() {
#if defined(__APPLE__)
    char buf[1024];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        std::string p(buf);
        size_t last_slash = p.rfind('/');
        if (last_slash != std::string::npos) return p.substr(0, last_slash);
    }
    return "";
#elif defined(_WIN32)
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, buf, MAX_PATH);
    if (len > 0) {
        int ulen = WideCharToMultiByte(CP_UTF8, 0, buf, len, NULL, 0, NULL, NULL);
        if (ulen > 0) {
            std::string u8(ulen, 0);
            WideCharToMultiByte(CP_UTF8, 0, buf, len, &u8[0], ulen, NULL, NULL);
            for (char &c : u8) if (c == '\\') c = '/';
            size_t last_slash = u8.rfind('/');
            if (last_slash != std::string::npos) return u8.substr(0, last_slash);
        }
    }
    return "";
#else
    char buf[1024];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        std::string p(buf);
        size_t last_slash = p.rfind('/');
        if (last_slash != std::string::npos) return p.substr(0, last_slash);
    }
    return "";
#endif
}

inline bool is_packaged() {
    const char *env = getenv("TAURILUA_DEV");
    if (env && env[0] != '\0' && strcmp(env, "0") != 0) return false;

    std::string exe_dir = get_executable_dir();
    if (exe_dir.find(".app/Contents/MacOS") != std::string::npos) {
        if (exe_dir.find("/build/") != std::string::npos) return false;
        return true;
    }
    if (exe_dir.rfind("/usr/bin", 0) == 0 || exe_dir.rfind("/usr/local/bin", 0) == 0) return true;
    return false;
}

inline bool is_dev_mode(bool config_dev_mode = false) {
    const char *env = getenv("TAURILUA_DEV");
    if (env && env[0] != '\0' && strcmp(env, "0") != 0) return true;
    if (config_dev_mode) return true;
    if (is_packaged()) return false;
    return false;
}

inline bool file_exists(const std::string &path) {
    if (path.empty()) return false;
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

inline std::string read_file_string(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

inline std::string resolve_bundle_resource(const std::string &rel_path) {
    std::string exe_dir = get_executable_dir();
    if (exe_dir.empty()) return "";

#if defined(__APPLE__)
    std::string res_dir = exe_dir + "/../Resources/" + rel_path;
    if (file_exists(res_dir)) return res_dir;
#endif

    std::string candidate1 = exe_dir + "/resources/" + rel_path;
    if (file_exists(candidate1)) return candidate1;

    std::string candidate2 = exe_dir + "/../share/" + rel_path;
    if (file_exists(candidate2)) return candidate2;

    std::string candidate3 = exe_dir + "/" + rel_path;
    if (file_exists(candidate3)) return candidate3;

    return "";
}

inline std::string resolve_main_lua(bool dev_mode) {
    if (dev_mode) {
        if (file_exists("src-tauri/src/main.lua")) return "src-tauri/src/main.lua";
        if (file_exists("src/main.lua")) return "src/main.lua";
    }

    std::string bundle_main = resolve_bundle_resource("main.lua");
    if (!bundle_main.empty()) return bundle_main;

    if (file_exists("src-tauri/src/main.lua")) return "src-tauri/src/main.lua";
    if (file_exists("src/main.lua")) return "src/main.lua";

    return "";
}

inline std::string resolve_config_json(bool dev_mode, const unsigned char *embedded_json, size_t embedded_len) {
    if (dev_mode) {
        if (file_exists("src-tauri/tauri.conf.json")) return read_file_string("src-tauri/tauri.conf.json");
    }

    // In release mode (!dev_mode), strictly enforce embedded configuration from binary headers
    if (embedded_json && embedded_len > 0) {
        return std::string((const char *)embedded_json, embedded_len);
    }
    return "{\"package\":{\"name\":\"tauri-app\"}}";
}

inline std::string resolve_assets_root(bool dev_mode, const std::string &configured_dist) {
    if (dev_mode) {
        if (!configured_dist.empty() && file_exists(configured_dist + "/index.html")) {
            return configured_dist;
        }
        if (file_exists("dist/index.html")) return "dist";
        return configured_dist.empty() ? "dist" : configured_dist;
    }
    std::string bundle_assets = resolve_bundle_resource(configured_dist.empty() ? "dist" : configured_dist);
    if (!bundle_assets.empty()) return bundle_assets;

    return get_executable_dir() + "/dist";
}

inline std::string resolve_capabilities_dir(bool dev_mode) {
    if (dev_mode) {
        if (file_exists("src-tauri/capabilities")) return "src-tauri/capabilities";
        if (file_exists("capabilities")) return "capabilities";
    }
    std::string bundle_caps = resolve_bundle_resource("capabilities");
    if (!bundle_caps.empty()) return bundle_caps;
    return "";
}

} // namespace ResourceResolver

#endif
