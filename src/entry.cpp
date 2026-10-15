#include <iostream>
#include <fstream>
#include <string>
#include <lua.hpp>

#include "native_bridge.h"
#include "tauri_bc.h"
#include "main_bc.h"
#include "config_json.h"
#include "isolate_pool.hpp"

static lua_State *g_main_L = nullptr;
static IsolatePool *g_isolate_pool = nullptr;

static bool file_exists(const std::string &path) {
    std::ifstream f(path);
    return f.good();
}

static std::string read_file_string(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static std::string resolve_main_lua() {
    if (file_exists("src-tauri/src/main.lua")) return "src-tauri/src/main.lua";
    if (file_exists("src/main.lua")) return "src/main.lua";
    return "";
}

static std::string resolve_config_json() {
    if (file_exists("src-tauri/tauri.conf.json")) return read_file_string("src-tauri/tauri.conf.json");
    if (file_exists("tauri.conf.json")) return read_file_string("tauri.conf.json");
    return std::string((const char *)tauri_conf_json, tauri_conf_json_len);
}

static void on_bridge_ipc(const char *window_label, const char *seq, const char *req) {
    if (g_isolate_pool) {
        g_isolate_pool->enqueue(window_label, seq, req);
    }
}

static void on_bridge_tray(int id) {
    if (!g_main_L) return;
    lua_getglobal(g_main_L, "__TAURI_DISPATCH_TRAY__");
    if (lua_isfunction(g_main_L, -1)) {
        lua_pushinteger(g_main_L, id);
        if (lua_pcall(g_main_L, 1, 0, 0) != 0) {
            lua_pop(g_main_L, 1);
        }
    } else {
        lua_pop(g_main_L, 1);
    }
}

static void on_bridge_shortcut(const char *shortcut) {
    if (!g_main_L) return;
    lua_getglobal(g_main_L, "__TAURI_DISPATCH_SHORTCUT__");
    if (lua_isfunction(g_main_L, -1)) {
        lua_pushstring(g_main_L, shortcut);
        if (lua_pcall(g_main_L, 1, 0, 0) != 0) {
            lua_pop(g_main_L, 1);
        }
    } else {
        lua_pop(g_main_L, 1);
    }
}

static void on_bridge_deeplink(const char *url) {
    if (!g_main_L) return;
    lua_getglobal(g_main_L, "__TAURI_DISPATCH_DEEPLINK__");
    if (lua_isfunction(g_main_L, -1)) {
        lua_pushstring(g_main_L, url);
        if (lua_pcall(g_main_L, 1, 0, 0) != 0) {
            lua_pop(g_main_L, 1);
        }
    } else {
        lua_pop(g_main_L, 1);
    }
}

static void on_bridge_menu(const char *id) {
    if (!g_main_L) return;
    lua_getglobal(g_main_L, "__TAURI_DISPATCH_MENU__");
    if (lua_isfunction(g_main_L, -1)) {
        lua_pushstring(g_main_L, id ? id : "");
        if (lua_pcall(g_main_L, 1, 0, 0) != 0) {
            std::cerr << "Menu dispatch error: " << lua_tostring(g_main_L, -1) << std::endl;
            lua_pop(g_main_L, 1);
        }
    } else {
        lua_pop(g_main_L, 1);
    }
}

static int on_bridge_window_close(const char *label) {
    if (!g_main_L) return 0;
    lua_getglobal(g_main_L, "__TAURI_DISPATCH_WINDOW_CLOSE__");
    if (lua_isfunction(g_main_L, -1)) {
        lua_pushstring(g_main_L, label ? label : "main");
        if (lua_pcall(g_main_L, 1, 1, 0) == 0) {
            int prevent = lua_toboolean(g_main_L, -1);
            lua_pop(g_main_L, 1);
            return prevent ? 1 : 0;
        } else {
            std::cerr << "Window close dispatch error: " << lua_tostring(g_main_L, -1) << std::endl;
            lua_pop(g_main_L, 1);
        }
    } else {
        lua_pop(g_main_L, 1);
    }
    return 0;
}

static void on_bridge_single_instance(const char *payload_json) {
    if (!g_main_L) return;
    lua_getglobal(g_main_L, "__TAURI_DISPATCH_SINGLE_INSTANCE__");
    if (lua_isfunction(g_main_L, -1)) {
        lua_pushstring(g_main_L, payload_json ? payload_json : "{}");
        if (lua_pcall(g_main_L, 1, 0, 0) != 0) {
            std::cerr << "Single instance dispatch error: " << lua_tostring(g_main_L, -1) << std::endl;
            lua_pop(g_main_L, 1);
        }
    } else {
        lua_pop(g_main_L, 1);
    }
}

static void on_bridge_theme_changed(const char *theme) {
    if (!g_main_L) return;
    lua_getglobal(g_main_L, "__TAURI_DISPATCH_THEME__");
    if (lua_isfunction(g_main_L, -1)) {
        lua_pushstring(g_main_L, theme ? theme : "light");
        if (lua_pcall(g_main_L, 1, 0, 0) != 0) {
            std::cerr << "Theme dispatch error: " << lua_tostring(g_main_L, -1) << std::endl;
            lua_pop(g_main_L, 1);
        }
    } else {
        lua_pop(g_main_L, 1);
    }
}

static void on_bridge_file_drop(const char *label, const char *event_type, const char *payload_json) {
    if (!g_main_L) return;
    lua_getglobal(g_main_L, "__TAURI_DISPATCH_FILE_DROP__");
    if (lua_isfunction(g_main_L, -1)) {
        lua_pushstring(g_main_L, label ? label : "main");
        lua_pushstring(g_main_L, event_type ? event_type : "drop");
        lua_pushstring(g_main_L, payload_json ? payload_json : "{}");
        if (lua_pcall(g_main_L, 3, 0, 0) != 0) {
            std::cerr << "File drop dispatch error: " << lua_tostring(g_main_L, -1) << std::endl;
            lua_pop(g_main_L, 1);
        }
    } else {
        lua_pop(g_main_L, 1);
    }
}

#if !defined(_WIN32)
#include <unistd.h>
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
    // Check plugin format: "single-instance": { ... "enabled": true ... }
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

static int preload_tauri(lua_State *L) {
    if (file_exists("src/tauri/init.lua")) {
        if (luaL_loadfile(L, "src/tauri/init.lua") != 0) {
            return lua_error(L);
        }
    } else if (file_exists("src-tauri/src/tauri/init.lua")) {
        if (luaL_loadfile(L, "src-tauri/src/tauri/init.lua") != 0) {
            return lua_error(L);
        }
    } else {
        if (luaL_loadbuffer(L, (const char *)luaJIT_BC_tauri, sizeof(luaJIT_BC_tauri), "tauri") != 0) {
            return lua_error(L);
        }
    }
    lua_call(L, 0, 1);
    return 1;
}

#if defined(_WIN32)
#include <windows.h>
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    int argc = __argc;
    char **argv = __argv;
#else
int main(int argc, char **argv) {
#endif
    std::string config_content = resolve_config_json();
    std::string instance_payload = make_instance_payload(argc, argv);

    platform_single_instance_set_callback(on_bridge_single_instance);

    if (is_single_instance_enabled(config_content)) {
        std::string app_id = extract_app_identifier(config_content);
        int res = platform_single_instance_init(app_id.c_str(), instance_payload.c_str());
        if (res == 0) {
            // Secondary instance: args sent to primary running instance. Exit cleanly!
            return 0;
        }
    }

    bridge_init(on_bridge_ipc, on_bridge_tray, on_bridge_shortcut, on_bridge_deeplink);
    platform_menu_set_callback(on_bridge_menu);
    platform_window_set_close_callback(on_bridge_window_close);
    platform_theme_set_callback(on_bridge_theme_changed);
    platform_file_drop_set_callback(on_bridge_file_drop);

    g_isolate_pool = new IsolatePool();

    g_main_L = luaL_newstate();
    if (!g_main_L) return 1;

    luaL_openlibs(g_main_L);

    lua_getglobal(g_main_L, "package");
    lua_getfield(g_main_L, -1, "preload");
    lua_pushcfunction(g_main_L, preload_tauri);
    lua_setfield(g_main_L, -2, "tauri");
    lua_pop(g_main_L, 2);

    lua_pushlstring(g_main_L, config_content.data(), config_content.size());
    lua_setglobal(g_main_L, "__EMBEDDED_CONFIG_JSON__");

    lua_pushlstring(g_main_L, instance_payload.data(), instance_payload.size());
    lua_setglobal(g_main_L, "__TAURI_STARTUP_PAYLOAD__");

    lua_pushboolean(g_main_L, 0);
    lua_setglobal(g_main_L, "__IS_ISOLATE_WORKER__");

    std::string main_path = resolve_main_lua();
    bool dev_mode = !main_path.empty();
    bridge_set_dev_mode(dev_mode ? 1 : 0);
    lua_pushboolean(g_main_L, dev_mode);
    lua_setglobal(g_main_L, "__TAURI_DEV__");

    int load_res = dev_mode ? luaL_loadfile(g_main_L, main_path.c_str())
                            : luaL_loadbuffer(g_main_L, (const char *)luaJIT_BC_main, sizeof(luaJIT_BC_main), "main");

    if (load_res != 0) {
        std::cerr << lua_tostring(g_main_L, -1) << std::endl;
        lua_close(g_main_L);
        delete g_isolate_pool;
        return 1;
    }

    if (lua_pcall(g_main_L, 0, 0, 0) != 0) {
        std::cerr << lua_tostring(g_main_L, -1) << std::endl;
        lua_close(g_main_L);
        delete g_isolate_pool;
        return 1;
    }

    bridge_run_loop();

    platform_single_instance_cleanup();

    delete g_isolate_pool;
    g_isolate_pool = nullptr;

    lua_close(g_main_L);
    return 0;
}