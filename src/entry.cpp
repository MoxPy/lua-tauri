#include <iostream>
#include <fstream>
#include <string>

#include "taurilua.h"
#include "native_bridge.h"

#if !defined(_WIN32)
#include <unistd.h>
#endif

static bool file_exists(const std::string &path) {
    std::ifstream f(path);
    return f.good();
}

static std::string read_file_string(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

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
        json += "\"";
        for (const char *p = argv[i]; *p; ++p) {
            if (*p == '"') json += "\\\"";
            else if (*p == '\\') json += "\\\\";
            else if (*p == '\n') json += "\\n";
            else if (*p == '\r') json += "\\r";
            else if (*p == '\t') json += "\\t";
            else json += *p;
        }
        json += "\"";
    }
    json += "],\"cwd\":\"";
    for (char c : cwd) {
        if (c == '"') json += "\\\"";
        else if (c == '\\') json += "\\\\";
        else json += c;
    }
    json += "\"}";
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

#if defined(_WIN32)
#include <windows.h>
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    int argc = __argc;
    char **argv = __argv;
#else
int main(int argc, char **argv) {
#endif
    std::string config_content = "";
    if (file_exists("src-tauri/tauri.conf.json")) config_content = read_file_string("src-tauri/tauri.conf.json");
    else if (file_exists("tauri.conf.json")) config_content = read_file_string("tauri.conf.json");

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
    if (!config_content.empty()) {
        cfg.config_json = config_content.c_str();
    }

    taurilua_app_t *app = taurilua_create(&cfg);
    if (!app) return 1;

    int ret = taurilua_run(app);
    taurilua_destroy(app);
    return ret;
}