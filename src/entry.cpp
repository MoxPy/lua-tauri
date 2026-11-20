#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#include <signal.h>
#endif

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cstdio>
#include <cctype>

#include "taurilua.h"
#include "native_bridge.h"
#include "asset_resolver.hpp"
#include "json_parser.hpp"
#include "resource_resolver.hpp"
#include "config_json.h"

#if !defined(_WIN32)
static void handle_exit_signal(int sig) {
    (void)sig;
    platform_kill_child_processes();
    _exit(0);
}
#endif

static std::string make_instance_payload(int argc, char **argv) {
    std::string cwd = "";
#if defined(_WIN32)
    DWORD wlen = GetCurrentDirectoryW(0, NULL);
    if (wlen > 0) {
        std::vector<wchar_t> wbuf(wlen + 1, 0);
        DWORD got = GetCurrentDirectoryW(wlen, wbuf.data());
        if (got > 0 && got < wlen) {
            int ulen = WideCharToMultiByte(CP_UTF8, 0, wbuf.data(), static_cast<int>(got), NULL, 0, NULL, NULL);
            if (ulen > 0) {
                cwd.resize(static_cast<size_t>(ulen));
                WideCharToMultiByte(CP_UTF8, 0, wbuf.data(), static_cast<int>(got), &cwd[0], ulen, NULL, NULL);
            }
        }
    }
#else
    char cwd_buf[4096];
    if (getcwd(cwd_buf, sizeof(cwd_buf))) cwd = cwd_buf;
#endif

    std::string json = "{\"args\":[";
    for (int i = 0; i < argc; ++i) {
        if (i > 0) json += ",";
        json += json_escape_string(argv[i] ? argv[i] : "");
    }
    json += "],\"cwd\":" + json_escape_string(cwd) + "}";
    return json;
}

static bool is_single_instance_enabled(const JsonVal &root) {
    if (!root.is_object()) return false;
    auto check_node = [](const JsonVal &v) -> bool {
        if (v.is_bool()) return v.boolean();
        if (v.is_object() && v.has("enabled") && v["enabled"].is_bool()) {
            return v["enabled"].boolean();
        }
        return false;
    };
    if (root.has("plugins") && root["plugins"].is_object()) {
        const JsonVal &p = root["plugins"];
        if (p.has("single-instance") && check_node(p["single-instance"])) return true;
        if (p.has("single_instance") && check_node(p["single_instance"])) return true;
        if (p.has("singleInstance") && check_node(p["singleInstance"])) return true;
    }
    if (root.has("single-instance") && check_node(root["single-instance"])) return true;
    if (root.has("single_instance") && check_node(root["single_instance"])) return true;
    if (root.has("singleInstance") && check_node(root["singleInstance"])) return true;
    return false;
}

static std::string extract_app_identifier(const JsonVal &root) {
    std::string raw_id = "com.tauri.lua";
    if (root.is_object()) {
        if (root.has("identifier") && root["identifier"].is_string() && !root["identifier"].str().empty()) {
            raw_id = root["identifier"].str();
        } else if (root.has("bundle") && root["bundle"].is_object() &&
                   root["bundle"].has("identifier") && root["bundle"]["identifier"].is_string() &&
                   !root["bundle"]["identifier"].str().empty()) {
            raw_id = root["bundle"]["identifier"].str();
        }
    }
    std::string safe_id;
    for (unsigned char c : raw_id) {
        if (std::isalnum(c) || c == '.' || c == '_' || c == '-') {
            safe_id.push_back(static_cast<char>(c));
        } else {
            safe_id.push_back('_');
        }
    }
    if (safe_id.empty() || safe_id == "." || safe_id == ".." || safe_id.find("..") != std::string::npos) {
        return "com.tauri.lua";
    }
    return safe_id;
}

static std::string extract_product_name(const JsonVal &root) {
    if (!root.is_object()) return "";
    if (root.has("productName") && root["productName"].is_string()) return root["productName"].str();
    if (root.has("product_name") && root["product_name"].is_string()) return root["product_name"].str();
    if (root.has("package") && root["package"].is_object() && root["package"].has("name") && root["package"]["name"].is_string()) {
        return root["package"]["name"].str();
    }
    return "";
}

#if defined(_WIN32)
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow;
    int argc = __argc;
    char **argv = __argv;
#else
int main(int argc, char **argv) {
#endif
#if !defined(_WIN32)
    signal(SIGINT, handle_exit_signal);
    signal(SIGTERM, handle_exit_signal);
    signal(SIGHUP, handle_exit_signal);
#if !defined(__APPLE__)
    setenv("NO_AT_BRIDGE", "1", 0);
    setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", 0);
#endif
#endif
    bool dev_mode = ResourceResolver::is_dev_mode();
    std::string config_content = ResourceResolver::resolve_config_json(dev_mode, tauri_conf_json, tauri_conf_json_len);

    JsonVal config_root;
    parse_json_strict(config_content, config_root, 64);

    std::string product_name = extract_product_name(config_root);
    if (!product_name.empty()) {
        platform_set_app_name(product_name.c_str());
    }

    std::string instance_payload = make_instance_payload(argc, argv);

    if (is_single_instance_enabled(config_root)) {
        std::string app_id = extract_app_identifier(config_root);
        int res = platform_single_instance_init(app_id.c_str(), instance_payload.c_str());
        if (res == 0) {
            return 0;
        }
    }

    taurilua_config_t cfg;
    taurilua_config_init(&cfg);
    cfg.dev_mode = dev_mode ? 1 : 0;
    if (!config_content.empty()) {
        cfg.config_json = config_content.c_str();
    }

    taurilua_app_t *app = taurilua_create(&cfg);
    if (!app) return 1;
    taurilua_set_startup_payload(app, instance_payload.c_str());

    int ret = taurilua_run(app);
    taurilua_destroy(app);
#if !defined(_WIN32)
    platform_kill_child_processes();
    std::fflush(nullptr);
    _exit(ret);
#endif
    return ret;
}
