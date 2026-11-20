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
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    if (size == 0) return "";
    std::vector<char> buf(size + 1, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) == 0) {
        std::string p(buf.data());
        size_t last_slash = p.rfind('/');
        if (last_slash != std::string::npos) return p.substr(0, last_slash);
    }
    return "";
#elif defined(_WIN32)
    std::vector<wchar_t> buf(32768, 0);
    DWORD len = GetModuleFileNameW(NULL, buf.data(), static_cast<DWORD>(buf.size()));
    if (len > 0 && len < buf.size()) {
        int ulen = WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(len), NULL, 0, NULL, NULL);
        if (ulen > 0) {
            std::string u8(static_cast<size_t>(ulen), '\0');
            WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(len), &u8[0], ulen, NULL, NULL);
            for (char &c : u8) if (c == '\\') c = '/';
            size_t last_slash = u8.rfind('/');
            if (last_slash != std::string::npos) return u8.substr(0, last_slash);
        }
    }
    return "";
#else
    std::vector<char> buf(4096, '\0');
    ssize_t len = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (len > 0 && static_cast<size_t>(len) < buf.size() - 1) {
        buf[static_cast<size_t>(len)] = '\0';
        std::string p(buf.data());
        size_t last_slash = p.rfind('/');
        if (last_slash != std::string::npos) return p.substr(0, last_slash);
    }
    return "";
#endif
}

inline bool is_packaged() {
#if defined(TAURILUA_RELEASE_FUSE)
    return true;
#else
    const char *env = getenv("TAURILUA_DEV");
    if (env && env[0] != '\0' && strcmp(env, "0") != 0) return false;

    std::string exe_dir = get_executable_dir();
    if (exe_dir.find(".app/Contents/MacOS") != std::string::npos) {
        if (exe_dir.find("/build/") != std::string::npos) return false;
        return true;
    }
    if (exe_dir.rfind("/usr/bin", 0) == 0 || exe_dir.rfind("/usr/local/bin", 0) == 0) return true;
    return false;
#endif
}

inline bool is_dev_mode(bool config_dev_mode = false) {
#if defined(TAURILUA_RELEASE_FUSE)
    (void)config_dev_mode;
    return false;
#else
    const char *env = getenv("TAURILUA_DEV");
    if (env && env[0] != '\0' && strcmp(env, "0") != 0) return true;
    if (config_dev_mode) return true;
    return false;
#endif
}

inline bool file_exists(const std::string &path) {
    if (path.empty()) return false;
#if defined(__cpp_lib_filesystem) || (defined(_LIBCPP_STD_VER) && _LIBCPP_STD_VER >= 17) || (__cplusplus >= 201703L)
    std::error_code ec;
    return std::filesystem::exists(path, ec);
#else
    std::ifstream f(path);
    return f.good();
#endif
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

#if !defined(_WIN32) && !defined(__APPLE__)
    std::vector<char> exe_buf(4096, '\0');
    ssize_t elen = readlink("/proc/self/exe", exe_buf.data(), exe_buf.size() - 1);
    if (elen > 0 && static_cast<size_t>(elen) < exe_buf.size() - 1) {
        exe_buf[static_cast<size_t>(elen)] = '\0';
        std::string ep(exe_buf.data());
        size_t slash = ep.rfind('/');
        if (slash != std::string::npos) {
            std::string exe_name = ep.substr(slash + 1);
            std::string c_share1 = "/usr/share/" + exe_name + "/" + rel_path;
            if (file_exists(c_share1)) return c_share1;
            std::string c_share2 = exe_dir + "/../share/" + exe_name + "/" + rel_path;
            if (file_exists(c_share2)) return c_share2;
            std::string c_share3 = "/usr/local/share/" + exe_name + "/" + rel_path;
            if (file_exists(c_share3)) return c_share3;
        }
    }
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
    if (!dev_mode) {
        return "";
    }
    if (file_exists("src-tauri/src/main.lua")) return "src-tauri/src/main.lua";
    if (file_exists("src/main.lua")) return "src/main.lua";
    return "";
}

inline std::string resolve_config_json(bool dev_mode, const unsigned char *embedded_json, size_t embedded_len) {
    if (dev_mode) {
        if (file_exists("src-tauri/tauri.conf.json")) return read_file_string("src-tauri/tauri.conf.json");
    }

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

}

#endif
