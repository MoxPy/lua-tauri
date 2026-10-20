#include <iostream>
#include <fstream>
#include <string>
#include <cstring>
#include <mutex>
#include <lua.hpp>

#include "taurilua.h"
#include "native_bridge.h"
#include "isolate_pool.hpp"

#include "resource_resolver.hpp"
#include "asset_resolver.hpp"

struct taurilua_app {
    lua_State *L = nullptr;
    IsolatePool *isolate_pool = nullptr;
    std::string config_json;
    std::string script_path;
    std::string inline_script;
    bool dev_mode = false;
    bool is_running = false;
};

static taurilua_app *g_active_app = nullptr;
static std::recursive_mutex g_main_lua_mutex;

static std::string lua_quote_string(const std::string &str) {
    std::string out = "\"";
    for (char c : str) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if ((unsigned char)c < 32) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\%03d", (unsigned char)c);
            out += buf;
        } else {
            out += c;
        }
    }
    out += "\"";
    return out;
}

static void on_bridge_ipc(const char *window_label, const char *seq, const char *req) {
    if (g_active_app && g_active_app->isolate_pool) {
        if (!g_active_app->isolate_pool->enqueue(window_label, seq, req)) {
            platform_window_return_async(window_label, seq, 1, "{\"error\":\"IPC request rejected: queue full or payload exceeded 10MB limit\"}");
        }
    }
}

static void on_bridge_tray(int id) {
    std::lock_guard<std::recursive_mutex> lock(g_main_lua_mutex);
    if (!g_active_app || !g_active_app->L) return;
    lua_getglobal(g_active_app->L, "__TAURI_DISPATCH_TRAY__");
    if (lua_isfunction(g_active_app->L, -1)) {
        lua_pushinteger(g_active_app->L, id);
        if (lua_pcall(g_active_app->L, 1, 0, 0) != 0) {
            lua_pop(g_active_app->L, 1);
        }
    } else {
        lua_pop(g_active_app->L, 1);
    }
}

static void on_bridge_shortcut(const char *shortcut) {
    std::lock_guard<std::recursive_mutex> lock(g_main_lua_mutex);
    if (!g_active_app || !g_active_app->L) return;
    lua_getglobal(g_active_app->L, "__TAURI_DISPATCH_SHORTCUT__");
    if (lua_isfunction(g_active_app->L, -1)) {
        lua_pushstring(g_active_app->L, shortcut);
        if (lua_pcall(g_active_app->L, 1, 0, 0) != 0) {
            lua_pop(g_active_app->L, 1);
        }
    } else {
        lua_pop(g_active_app->L, 1);
    }
}

static void on_bridge_deeplink(const char *url) {
    std::lock_guard<std::recursive_mutex> lock(g_main_lua_mutex);
    if (!g_active_app || !g_active_app->L) return;
    lua_getglobal(g_active_app->L, "__TAURI_DISPATCH_DEEPLINK__");
    if (lua_isfunction(g_active_app->L, -1)) {
        lua_pushstring(g_active_app->L, url);
        if (lua_pcall(g_active_app->L, 1, 0, 0) != 0) {
            lua_pop(g_active_app->L, 1);
        }
    } else {
        lua_pop(g_active_app->L, 1);
    }
}

static void on_bridge_menu(const char *id) {
    std::lock_guard<std::recursive_mutex> lock(g_main_lua_mutex);
    if (!g_active_app || !g_active_app->L) return;
    lua_getglobal(g_active_app->L, "__TAURI_DISPATCH_MENU__");
    if (lua_isfunction(g_active_app->L, -1)) {
        lua_pushstring(g_active_app->L, id ? id : "");
        if (lua_pcall(g_active_app->L, 1, 0, 0) != 0) {
            std::cerr << "Menu dispatch error: " << lua_tostring(g_active_app->L, -1) << std::endl;
            lua_pop(g_active_app->L, 1);
        }
    } else {
        lua_pop(g_active_app->L, 1);
    }
}

static int on_bridge_window_close(const char *label) {
    std::lock_guard<std::recursive_mutex> lock(g_main_lua_mutex);
    if (!g_active_app || !g_active_app->L) return 0;
    lua_getglobal(g_active_app->L, "__TAURI_DISPATCH_WINDOW_CLOSE__");
    if (lua_isfunction(g_active_app->L, -1)) {
        lua_pushstring(g_active_app->L, label ? label : "main");
        if (lua_pcall(g_active_app->L, 1, 1, 0) == 0) {
            int prevent = lua_toboolean(g_active_app->L, -1);
            lua_pop(g_active_app->L, 1);
            return prevent ? 1 : 0;
        } else {
            std::cerr << "Window close dispatch error: " << lua_tostring(g_active_app->L, -1) << std::endl;
            lua_pop(g_active_app->L, 1);
        }
    } else {
        lua_pop(g_active_app->L, 1);
    }
    return 0;
}

static void on_bridge_single_instance(const char *payload_json) {
    std::lock_guard<std::recursive_mutex> lock(g_main_lua_mutex);
    if (!g_active_app || !g_active_app->L) return;
    lua_getglobal(g_active_app->L, "__TAURI_DISPATCH_SINGLE_INSTANCE__");
    if (lua_isfunction(g_active_app->L, -1)) {
        lua_pushstring(g_active_app->L, payload_json ? payload_json : "{}");
        if (lua_pcall(g_active_app->L, 1, 0, 0) != 0) {
            std::cerr << "Single instance dispatch error: " << lua_tostring(g_active_app->L, -1) << std::endl;
            lua_pop(g_active_app->L, 1);
        }
    } else {
        lua_pop(g_active_app->L, 1);
    }
}

static void on_bridge_theme_changed(const char *theme) {
    std::lock_guard<std::recursive_mutex> lock(g_main_lua_mutex);
    if (!g_active_app || !g_active_app->L) return;
    lua_getglobal(g_active_app->L, "__TAURI_DISPATCH_THEME__");
    if (lua_isfunction(g_active_app->L, -1)) {
        lua_pushstring(g_active_app->L, theme ? theme : "light");
        if (lua_pcall(g_active_app->L, 1, 0, 0) != 0) {
            std::cerr << "Theme dispatch error: " << lua_tostring(g_active_app->L, -1) << std::endl;
            lua_pop(g_active_app->L, 1);
        }
    } else {
        lua_pop(g_active_app->L, 1);
    }
}

static void on_bridge_file_drop(const char *label, const char *event_type, const char *payload_json) {
    std::lock_guard<std::recursive_mutex> lock(g_main_lua_mutex);
    if (!g_active_app || !g_active_app->L) return;
    lua_getglobal(g_active_app->L, "__TAURI_DISPATCH_FILE_DROP__");
    if (lua_isfunction(g_active_app->L, -1)) {
        lua_pushstring(g_active_app->L, label ? label : "main");
        lua_pushstring(g_active_app->L, event_type ? event_type : "drop");
        lua_pushstring(g_active_app->L, payload_json ? payload_json : "{}");
        if (lua_pcall(g_active_app->L, 3, 0, 0) != 0) {
            std::cerr << "File drop dispatch error: " << lua_tostring(g_active_app->L, -1) << std::endl;
            lua_pop(g_active_app->L, 1);
        }
    } else {
        lua_pop(g_active_app->L, 1);
    }
}

static int preload_tauri(lua_State *L) {
    bool dev_mode = g_active_app ? g_active_app->dev_mode : ResourceResolver::is_dev_mode();
    if (dev_mode) {
        const char *engine_root = getenv("TAURI_LUA_ROOT");
        if (engine_root && engine_root[0] != 0) {
            std::string p = std::string(engine_root) + "/src/tauri/init.lua";
            if (ResourceResolver::file_exists(p)) {
                if (luaL_loadfile(L, p.c_str()) != 0) {
                    return lua_error(L);
                }
                lua_call(L, 0, 1);
                return 1;
            }
        }
        if (ResourceResolver::file_exists("src/tauri/init.lua")) {
            if (luaL_loadfile(L, "src/tauri/init.lua") != 0) {
                return lua_error(L);
            }
            lua_call(L, 0, 1);
            return 1;
        } else if (ResourceResolver::file_exists("src-tauri/src/tauri/init.lua")) {
            if (luaL_loadfile(L, "src-tauri/src/tauri/init.lua") != 0) {
                return lua_error(L);
            }
            lua_call(L, 0, 1);
            return 1;
        }
    }
    if (luaL_loadbuffer(L, (const char *)luaJIT_BC_tauri, sizeof(luaJIT_BC_tauri), "tauri") != 0) {
        return lua_error(L);
    }
    lua_call(L, 0, 1);
    return 1;
}

extern "C" {

void taurilua_config_init(taurilua_config_t *config) {
    if (!config) return;
    config->app_name = "tauri-lua-app";
    config->title = "Tauri-Lua Host";
    config->url = "";
    config->width = 1024;
    config->height = 720;
    config->resizable = 1;
    config->dev_mode = 0;
    config->config_json = nullptr;
    config->custom_script = nullptr;
}

taurilua_app_t *taurilua_create(const taurilua_config_t *config) {
    taurilua_app *app = new taurilua_app();
    g_active_app = app;

    app->dev_mode = config ? (config->dev_mode != 0) : ResourceResolver::is_dev_mode();

    if (config && config->config_json && strlen(config->config_json) > 0) {
        app->config_json = config->config_json;
    } else {
        app->config_json = ResourceResolver::resolve_config_json(app->dev_mode, tauri_conf_json, tauri_conf_json_len);
    }

    if (config && config->custom_script && strlen(config->custom_script) > 0) {
        app->script_path = config->custom_script;
    }

    // Initialize platform bridge callbacks
    bridge_init(on_bridge_ipc, on_bridge_tray, on_bridge_shortcut, on_bridge_deeplink);
    platform_menu_set_callback(on_bridge_menu);
    platform_window_set_close_callback(on_bridge_window_close);
    platform_theme_set_callback(on_bridge_theme_changed);
    platform_file_drop_set_callback(on_bridge_file_drop);
    platform_single_instance_set_callback(on_bridge_single_instance);
    bridge_set_dev_mode(app->dev_mode ? 1 : 0);
    if (app->dev_mode && !app->config_json.empty()) {
        size_t p = app->config_json.find("devUrl");
        if (p != std::string::npos) {
            size_t colon = app->config_json.find(':', p);
            if (colon != std::string::npos) {
                size_t q1 = app->config_json.find('"', colon);
                if (q1 != std::string::npos) {
                    size_t q2 = app->config_json.find('"', q1 + 1);
                    if (q2 != std::string::npos) {
                        std::string dev_url = app->config_json.substr(q1 + 1, q2 - q1 - 1);
                        bridge_set_dev_url(dev_url.c_str());
                    }
                }
            }
        }
    }

    // Initialize isolate thread pool
    app->isolate_pool = new IsolatePool();

    // Initialize primary Lua state
    app->L = luaL_newstate();
    if (!app->L) {
        delete app->isolate_pool;
        delete app;
        g_active_app = nullptr;
        return nullptr;
    }

    luaL_openlibs(app->L);

    // Setup package.preload["tauri"]
    lua_getglobal(app->L, "package");
    lua_getfield(app->L, -1, "preload");
    lua_pushcfunction(app->L, preload_tauri);
    lua_setfield(app->L, -2, "tauri");
    lua_pop(app->L, 2);

    // Inject configuration and state globals
    lua_pushlstring(app->L, app->config_json.data(), app->config_json.size());
    lua_setglobal(app->L, "__EMBEDDED_CONFIG_JSON__");

    lua_pushstring(app->L, "{\"args\":[]}");
    lua_setglobal(app->L, "__TAURI_STARTUP_PAYLOAD__");

    lua_pushboolean(app->L, 0);
    lua_setglobal(app->L, "__IS_ISOLATE_WORKER__");

    lua_pushboolean(app->L, app->dev_mode ? 1 : 0);
    lua_setglobal(app->L, "__TAURI_DEV__");

    return app;
}

taurilua_app_t *taurilua_create_from_file(const char *script_path, const char *config_json) {
    taurilua_config_t cfg;
    taurilua_config_init(&cfg);
    cfg.custom_script = script_path;
    cfg.config_json = config_json;
    return taurilua_create(&cfg);
}

taurilua_app_t *taurilua_create_from_string(const char *lua_source, const char *config_json) {
    taurilua_config_t cfg;
    taurilua_config_init(&cfg);
    cfg.config_json = config_json;
    taurilua_app_t *app = taurilua_create(&cfg);
    if (app && lua_source) {
        app->inline_script = lua_source;
    }
    return app;
}

int taurilua_run(taurilua_app_t *app) {
    if (!app || !app->L) return -1;
    g_active_app = app;
    app->is_running = true;

    int load_res = 0;
    if (!app->inline_script.empty()) {
        load_res = luaL_loadbuffer(app->L, app->inline_script.data(), app->inline_script.size(), "host_inline");
    } else if (!app->script_path.empty() && ResourceResolver::file_exists(app->script_path)) {
        load_res = luaL_loadfile(app->L, app->script_path.c_str());
    } else {
        std::string resolved = ResourceResolver::resolve_main_lua(app->dev_mode);
        if (!resolved.empty() && app->dev_mode) {
            load_res = luaL_loadfile(app->L, resolved.c_str());
        } else {
            load_res = luaL_loadbuffer(app->L, (const char *)luaJIT_BC_main, sizeof(luaJIT_BC_main), "main");
        }
    }

    if (load_res != 0) {
        std::cerr << "Tauri-Lua load error: " << lua_tostring(app->L, -1) << std::endl;
        return 1;
    }

    if (lua_pcall(app->L, 0, 0, 0) != 0) {
        std::cerr << "Tauri-Lua runtime error: " << lua_tostring(app->L, -1) << std::endl;
        return 1;
    }

    bridge_run_loop();
    app->is_running = false;
    return 0;
}

void taurilua_terminate(taurilua_app_t *app) {
    (void)app;
    bridge_terminate();
}

void taurilua_destroy(taurilua_app_t *app) {
    if (!app) return;
    if (app->is_running) {
        taurilua_terminate(app);
    }
    platform_single_instance_cleanup();
    if (app->isolate_pool) {
        delete app->isolate_pool;
        app->isolate_pool = nullptr;
    }
    if (app->L) {
        lua_close(app->L);
        app->L = nullptr;
    }
    if (g_active_app == app) {
        g_active_app = nullptr;
    }
    delete app;
}

struct lua_State *taurilua_get_lua_state(taurilua_app_t *app) {
    return app ? app->L : nullptr;
}

int taurilua_eval_lua(taurilua_app_t *app, const char *lua_code) {
    if (!app || !app->L || !lua_code) return -1;
    int res = luaL_dostring(app->L, lua_code);
    if (res != 0) {
        std::cerr << "[taurilua_eval_lua error]: " << lua_tostring(app->L, -1) << std::endl;
        lua_pop(app->L, 1);
    }
    return res;
}

void taurilua_eval_js(taurilua_app_t *app, const char *window_label, const char *javascript) {
    (void)app;
    if (window_label && javascript) {
        platform_window_eval(window_label, javascript);
    }
}

void taurilua_emit(taurilua_app_t *app, const char *event_name, const char *payload_json) {
    std::lock_guard<std::recursive_mutex> lock(g_main_lua_mutex);
    if (!app || !app->L || !event_name) return;
    lua_getglobal(app->L, "__TAURI_DISPATCH_EMIT__");
    if (lua_isfunction(app->L, -1)) {
        lua_pushstring(app->L, event_name);
        lua_pushstring(app->L, payload_json ? payload_json : "{}");
        if (lua_pcall(app->L, 2, 0, 0) != 0) {
            lua_pop(app->L, 1);
        }
    } else {
        lua_pop(app->L, 1);
        // Fallback: evaluate window.__TAURI__.event.emit across windows safely quoted
        std::string safe_event = json_escape_string(event_name);
        std::string safe_payload = (payload_json && payload_json[0] != '\0') ? payload_json : "{}";
        std::string js = std::string("if(window.__TAURI__&&window.__TAURI__.event){window.__TAURI__.event.emit(") +
                         safe_event + "," + safe_payload + ");}";
        platform_window_eval("main", js.c_str());
    }
}

int taurilua_open_window(const char *title, const char *url, int width, int height) {
    std::string safe_title = lua_quote_string(title ? title : "Tauri-Lua Window");
    std::string safe_url = lua_quote_string(url ? url : "");
    int w = width > 0 ? width : 1024;
    int h = height > 0 ? height : 720;

    std::string script = "local Tauri = require('tauri')\n"
                         "local app = Tauri.init({\n"
                         "    app = { windows = { {\n"
                         "        title = " + safe_title + ",\n"
                         "        url = " + safe_url + ",\n"
                         "        width = " + std::to_string(w) + ",\n"
                         "        height = " + std::to_string(h) + ",\n"
                         "        resizable = true\n"
                         "    } } }\n"
                         "})\n";

    taurilua_app_t *app = taurilua_create_from_string(script.c_str(), nullptr);
    if (!app) return -1;
    int res = taurilua_run(app);
    taurilua_destroy(app);
    return res;
}

} // extern "C"
