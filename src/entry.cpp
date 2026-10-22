#include <iostream>
#include <fstream>
#include <string>

#include "taurilua.h"
#include "native_bridge.h"
#include "asset_resolver.hpp"

#if !defined(_WIN32)
#include <unistd.h>
#include <signal.h>

static void handle_exit_signal(int sig) {
    (void)sig;
    platform_kill_child_processes();
    _exit(0);
}
#endif


static std::string make_instance_payload(int argc, char **argv) {
    std::string cwd = "";
#if defined(_WIN32)
    char cwd_buf[MAX_PATH];
    if (GetCurrentDirectoryA(MAX_PATH, cwd_buf)) cwd = cwd_buf;
#else
    char cwd_buf[1024];
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

static bool is_single_instance_enabled(const std::string &conf) {
    if (conf.find("\"singleInstance\": true") != std::string::npos ||
        conf.find("\"singleInstance\":true") != std::string::npos ||
        conf.find("\"single_instance\": true") != std::string::npos ||
        conf.find("\"single_instance\":true") != std::string::npos ||
        conf.find("\"single-instance\": true") != std::string::npos ||
        conf.find("\"single-instance\":true") != std::string::npos) {
        return true;
    }
    size_t pos = conf.find("\"single-instance\"");
    if (pos == std::string::npos) pos = conf.find("\"single_instance\"");
    if (pos == std::string::npos) pos = conf.find("\"singleInstance\"");
    if (pos != std::string::npos) {
        size_t brace = conf.find('{', pos);
        size_t close_brace = conf.find('}', pos);
        if (brace != std::string::npos && (close_brace == std::string::npos || brace < close_brace)) {
            std::string block = conf.substr(brace, close_brace - brace + 1);
            if (block.find("\"enabled\": true") != std::string::npos ||
                block.find("\"enabled\":true") != std::string::npos) {
                return true;
            }
        }
    }
    return false;
}

static std::string extract_app_identifier(const std::string &conf) {
    size_t pos = conf.find("\"identifier\"");
    if (pos != std::string::npos) {
        size_t colon = conf.find(':', pos);
        if (colon != std::string::npos) {
            size_t q1 = conf.find('"', colon);
            if (q1 != std::string::npos) {
                size_t q2 = conf.find('"', q1 + 1);
                if (q2 != std::string::npos) {
                    return conf.substr(q1 + 1, q2 - q1 - 1);
                }
            }
        }
    }
    return "com.tauri.lua";
}

static std::string extract_product_name(const std::string &conf) {
    size_t pos = conf.find("\"productName\"");
    if (pos == std::string::npos) pos = conf.find("\"product_name\"");
    if (pos != std::string::npos) {
        size_t colon = conf.find(':', pos);
        if (colon != std::string::npos) {
            size_t q1 = conf.find('"', colon);
            if (q1 != std::string::npos) {
                size_t q2 = conf.find('"', q1 + 1);
                if (q2 != std::string::npos) {
                    return conf.substr(q1 + 1, q2 - q1 - 1);
                }
            }
        }
    }
    pos = conf.find("\"package\"");
    if (pos != std::string::npos) {
        size_t name_pos = conf.find("\"name\"", pos);
        if (name_pos != std::string::npos) {
            size_t colon = conf.find(':', name_pos);
            if (colon != std::string::npos) {
                size_t q1 = conf.find('"', colon);
                if (q1 != std::string::npos) {
                    size_t q2 = conf.find('"', q1 + 1);
                    if (q2 != std::string::npos) {
                        return conf.substr(q1 + 1, q2 - q1 - 1);
                    }
                }
            }
        }
    }
    return "";
}

#include "resource_resolver.hpp"
#include "config_json.h"

#if defined(_WIN32)
#include <windows.h>
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
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
    // Prevent harmless GTK accessibility bus warning when AT-SPI DBus daemon is not running (e.g. ChromeOS Crostini, Docker, minimal Linux)
    setenv("NO_AT_BRIDGE", "1", 0);
#endif
#endif
    bool dev_mode = ResourceResolver::is_dev_mode();
    std::string config_content = ResourceResolver::resolve_config_json(dev_mode, tauri_conf_json, tauri_conf_json_len);

    std::string product_name = extract_product_name(config_content);
    if (!product_name.empty()) {
        platform_set_app_name(product_name.c_str());
    }

    std::string instance_payload = make_instance_payload(argc, argv);

    if (is_single_instance_enabled(config_content)) {
        std::string app_id = extract_app_identifier(config_content);
        int res = platform_single_instance_init(app_id.c_str(), instance_payload.c_str());
        if (res == 0) {
            // Secondary instance: payload sent to primary instance. Exit cleanly.
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

    int ret = taurilua_run(app);
    taurilua_destroy(app);
#if !defined(_WIN32)
    platform_kill_child_processes();
    _exit(ret);
#endif
    return ret;
}