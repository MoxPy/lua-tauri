#include <iostream>
#include <fstream>
#include <string>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <lua.hpp>

#include "taurilua.h"
#include "native_bridge.h"
#include "isolate_pool.hpp"

#include "resource_resolver.hpp"
#include "asset_resolver.hpp"
#include "json_parser.hpp"

struct HostCommandEntry {
    taurilua_command_fn fn = nullptr;
    void *user_data = nullptr;
};

struct taurilua_app {
    lua_State *L = nullptr;
    IsolatePool *isolate_pool = nullptr;
    std::string config_json;
    std::string script_path;
    std::string inline_script;
    std::string startup_payload = "{\"args\":[]}";
    std::string app_name;
    std::string window_title;
    std::string window_url;
    int window_width = 0;
    int window_height = 0;
    int window_resizable = -1;
    bool dev_mode = false;
    bool is_running = false;
    std::mutex commands_mutex;
    std::unordered_map<std::string, HostCommandEntry> host_commands;
    std::mutex error_mutex;
    std::string last_error;
};

static taurilua_app *g_active_app = nullptr;
static std::mutex g_app_lifecycle_mutex;
static std::recursive_mutex g_main_lua_mutex;

static void set_app_error(taurilua_app *app, const std::string &err) {
    if (!app) return;
    std::lock_guard<std::mutex> lock(app->error_mutex);
    app->last_error = err;
}

static std::string lua_quote_string(const std::string &str) {
    std::string out = "\"";
    for (char c : str) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (static_cast<unsigned char>(c) < 32) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\%03d", static_cast<unsigned char>(c));
            out += buf;
        } else {
            out += c;
        }
    }
    out += "\"";
    return out;
}

static int lua_invoke_host_command(lua_State *L) {
    taurilua_app *app = static_cast<taurilua_app *>(lua_touserdata(L, lua_upvalueindex(1)));
    const char *cmd = luaL_checkstring(L, 1);
    const char *payload_json = luaL_optstring(L, 2, "{}");
    const char *label = luaL_optstring(L, 3, "main");
    if (!app || !cmd) {
        lua_pushboolean(L, 0);
        return 1;
    }

    HostCommandEntry entry;
    {
        std::lock_guard<std::mutex> lock(app->commands_mutex);
        auto it = app->host_commands.find(cmd);
        if (it == app->host_commands.end() || !it->second.fn) {
            lua_pushboolean(L, 0);
            return 1;
        }
        entry = it->second;
    }

    std::vector<char> out_buf(65536, '\0');
    int is_error = 0;
    entry.fn(payload_json, label, out_buf.data(), out_buf.size(), &is_error, entry.user_data);
    out_buf.back() = '\0';

    lua_pushboolean(L, 1);
    lua_pushboolean(L, is_error != 0 ? 1 : 0);
    lua_pushstring(L, out_buf.data());
    return 3;
}

static void on_bridge_ipc(const char *window_label, const char *seq, const char *req) {
    std::lock_guard<std::mutex> lock(g_app_lifecycle_mutex);
    if (g_active_app && g_active_app->isolate_pool) {
        if (!g_active_app->isolate_pool->enqueue(window_label ? window_label : "main", seq ? seq : "", req ? req : "")) {
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
#if !defined(TAURILUA_RELEASE_FUSE)
    lua_getglobal(L, "__TAURI_DEV__");
    bool dev_mode = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
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
#endif
    if (luaL_loadbuffer(L, reinterpret_cast<const char *>(luaJIT_BC_tauri), sizeof(luaJIT_BC_tauri), "tauri") != 0) {
        return lua_error(L);
    }
    lua_call(L, 0, 1);
    return 1;
}

static void push_host_overrides(lua_State *L, const taurilua_app *app) {
    lua_newtable(L);
    if (!app->app_name.empty()) {
        lua_pushlstring(L, app->app_name.data(), app->app_name.size());
        lua_setfield(L, -2, "app_name");
    }
    if (!app->window_title.empty()) {
        lua_pushlstring(L, app->window_title.data(), app->window_title.size());
        lua_setfield(L, -2, "title");
    }
    if (!app->window_url.empty()) {
        lua_pushlstring(L, app->window_url.data(), app->window_url.size());
        lua_setfield(L, -2, "url");
    }
    if (app->window_width > 0) {
        lua_pushinteger(L, app->window_width);
        lua_setfield(L, -2, "width");
    }
    if (app->window_height > 0) {
        lua_pushinteger(L, app->window_height);
        lua_setfield(L, -2, "height");
    }
    if (app->window_resizable >= 0) {
        lua_pushboolean(L, app->window_resizable != 0 ? 1 : 0);
        lua_setfield(L, -2, "resizable");
    }
    lua_setglobal(L, "__TAURI_HOST_OVERRIDES__");
}

static taurilua_app_t *taurilua_create_internal(const taurilua_config_t *config, const char *inline_script) {
    taurilua_app *app = new taurilua_app();

#if defined(TAURILUA_RELEASE_FUSE)
    app->dev_mode = false;
#else
    app->dev_mode = config ? (config->dev_mode != 0) : ResourceResolver::is_dev_mode();
#endif

    if (config && config->app_name && config->app_name[0] != '\0') {
        app->app_name = config->app_name;
        platform_set_app_name(app->app_name.c_str());
    }
    if (config && config->title && config->title[0] != '\0') {
        app->window_title = config->title;
    }
    if (config && config->url && config->url[0] != '\0') {
        app->window_url = config->url;
    }
    if (config && config->width > 0) {
        app->window_width = config->width;
    }
    if (config && config->height > 0) {
        app->window_height = config->height;
    }
    if (config) {
        app->window_resizable = config->resizable ? 1 : 0;
    }

    if (config && config->config_json && strlen(config->config_json) > 0) {
        app->config_json = config->config_json;
    } else {
        app->config_json = ResourceResolver::resolve_config_json(app->dev_mode, tauri_conf_json, tauri_conf_json_len);
    }

    if (config && config->custom_script && strlen(config->custom_script) > 0) {
        app->script_path = config->custom_script;
    }
    if (inline_script && inline_script[0] != '\0') {
        app->inline_script = inline_script;
    }

    bridge_init(on_bridge_ipc, on_bridge_tray, on_bridge_shortcut, on_bridge_deeplink);
    platform_menu_set_callback(on_bridge_menu);
    platform_window_set_close_callback(on_bridge_window_close);
    platform_theme_set_callback(on_bridge_theme_changed);
    platform_file_drop_set_callback(on_bridge_file_drop);
    platform_single_instance_set_callback(on_bridge_single_instance);
    bridge_set_dev_mode(app->dev_mode ? 1 : 0);

    if (app->dev_mode && !app->config_json.empty()) {
        JsonVal cfg_root;
        if (parse_json_strict(app->config_json, cfg_root, 64) && cfg_root.is_object()) {
            std::string dev_url;
            if (cfg_root.has("build") && cfg_root["build"].is_object()) {
                const JsonVal &b = cfg_root["build"];
                if (b.has("devUrl") && b["devUrl"].is_string()) dev_url = b["devUrl"].str();
                else if (b.has("devPath") && b["devPath"].is_string()) dev_url = b["devPath"].str();
            }
            if (!dev_url.empty()) {
                bridge_set_dev_url(dev_url.c_str());
            }
        }
    }

    IsolateConfig iso_cfg;
    iso_cfg.config_json = app->config_json;
    iso_cfg.script_path = app->script_path;
    iso_cfg.inline_script = app->inline_script;
    iso_cfg.startup_payload = app->startup_payload;
    iso_cfg.dev_mode = app->dev_mode;
    iso_cfg.host_cmd_dispatcher = lua_invoke_host_command;
    iso_cfg.host_cmd_userdata = app;
    app->isolate_pool = new IsolatePool(iso_cfg);

    app->L = luaL_newstate();
    if (!app->L) {
        delete app->isolate_pool;
        delete app;
        return nullptr;
    }

    luaL_openlibs(app->L);

    lua_getglobal(app->L, "package");
    lua_getfield(app->L, -1, "preload");
    lua_pushcfunction(app->L, preload_tauri);
    lua_setfield(app->L, -2, "tauri");
    lua_pop(app->L, 2);

    lua_pushlstring(app->L, app->config_json.data(), app->config_json.size());
    lua_setglobal(app->L, "__EMBEDDED_CONFIG_JSON__");

    lua_pushlstring(app->L, app->startup_payload.data(), app->startup_payload.size());
    lua_setglobal(app->L, "__TAURI_STARTUP_PAYLOAD__");

    lua_pushboolean(app->L, 0);
    lua_setglobal(app->L, "__IS_ISOLATE_WORKER__");

    lua_pushboolean(app->L, app->dev_mode ? 1 : 0);
    lua_setglobal(app->L, "__TAURI_DEV__");

    lua_pushlightuserdata(app->L, app);
    lua_pushcclosure(app->L, lua_invoke_host_command, 1);
    lua_setglobal(app->L, "__TAURI_INVOKE_HOST_CMD__");

    push_host_overrides(app->L, app);

    {
        std::lock_guard<std::mutex> lock(g_app_lifecycle_mutex);
        g_active_app = app;
    }

    return app;
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
    return taurilua_create_internal(config, nullptr);
}

taurilua_app_t *taurilua_create_from_file(const char *script_path, const char *config_json) {
    taurilua_config_t cfg;
    taurilua_config_init(&cfg);
    cfg.custom_script = script_path;
    cfg.config_json = config_json;
    return taurilua_create_internal(&cfg, nullptr);
}

taurilua_app_t *taurilua_create_from_string(const char *lua_source, const char *config_json) {
    taurilua_config_t cfg;
    taurilua_config_init(&cfg);
    cfg.config_json = config_json;
    return taurilua_create_internal(&cfg, lua_source);
}

void taurilua_set_startup_payload(taurilua_app_t *app, const char *payload_json) {
    if (!app || !payload_json) return;
    std::lock_guard<std::recursive_mutex> lock(g_main_lua_mutex);
    app->startup_payload = payload_json;
    if (app->L) {
        lua_pushlstring(app->L, app->startup_payload.data(), app->startup_payload.size());
        lua_setglobal(app->L, "__TAURI_STARTUP_PAYLOAD__");
    }
}

int taurilua_register_command(taurilua_app_t *app, const char *command_name, taurilua_command_fn handler, void *user_data) {
    if (!app || !command_name || command_name[0] == '\0' || !handler) return -1;
    std::lock_guard<std::mutex> lock(app->commands_mutex);
    app->host_commands[command_name] = HostCommandEntry{handler, user_data};
    return 0;
}

const char *taurilua_last_error(taurilua_app_t *app) {
    if (!app) return "";
    std::lock_guard<std::mutex> lock(app->error_mutex);
    return app->last_error.c_str();
}

int taurilua_run(taurilua_app_t *app) {
    if (!app || !app->L) return -1;
    {
        std::lock_guard<std::mutex> lock(g_app_lifecycle_mutex);
        g_active_app = app;
    }
    app->is_running = true;

    int load_res = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_main_lua_mutex);
        if (!app->inline_script.empty()) {
            load_res = luaL_loadbuffer(app->L, app->inline_script.data(), app->inline_script.size(), "host_inline");
        } else if (!app->script_path.empty() && ResourceResolver::file_exists(app->script_path)) {
            load_res = luaL_loadfile(app->L, app->script_path.c_str());
        } else {
            std::string resolved = ResourceResolver::resolve_main_lua(app->dev_mode);
            if (!resolved.empty()) {
                load_res = luaL_loadfile(app->L, resolved.c_str());
            } else {
                load_res = luaL_loadbuffer(app->L, reinterpret_cast<const char *>(luaJIT_BC_main), sizeof(luaJIT_BC_main), "main");
            }
        }

        if (load_res != 0) {
            const char *msg = lua_tostring(app->L, -1);
            std::string err_str = msg ? msg : "unknown load error";
            set_app_error(app, err_str);
            std::cerr << "Tauri-Lua load error: " << err_str << std::endl;
            lua_pop(app->L, 1);
            app->is_running = false;
            return 1;
        }

        if (lua_pcall(app->L, 0, 0, 0) != 0) {
            const char *msg = lua_tostring(app->L, -1);
            std::string err_str = msg ? msg : "unknown runtime error";
            set_app_error(app, err_str);
            std::cerr << "Tauri-Lua runtime error: " << err_str << std::endl;
            lua_pop(app->L, 1);
            app->is_running = false;
            return 1;
        }
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
    IsolatePool *pool_to_delete = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_app_lifecycle_mutex);
        if (g_active_app == app) {
            g_active_app = nullptr;
        }
        pool_to_delete = app->isolate_pool;
        app->isolate_pool = nullptr;
    }
    if (pool_to_delete) {
        delete pool_to_delete;
    }
    {
        std::lock_guard<std::recursive_mutex> lock(g_main_lua_mutex);
        if (app->L) {
            lua_close(app->L);
            app->L = nullptr;
        }
    }
    delete app;
}

struct lua_State *taurilua_get_lua_state(taurilua_app_t *app) {
    return app ? app->L : nullptr;
}

int taurilua_eval_lua(taurilua_app_t *app, const char *lua_code) {
    std::lock_guard<std::recursive_mutex> lock(g_main_lua_mutex);
    if (!app || !app->L || !lua_code) return -1;
    int res = luaL_dostring(app->L, lua_code);
    if (res != 0) {
        const char *msg = lua_tostring(app->L, -1);
        std::string err_str = msg ? msg : "unknown eval error";
        set_app_error(app, err_str);
        std::cerr << "[taurilua_eval_lua error]: " << err_str << std::endl;
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
                         "})\n"
                         "app:create_main_window()\n";

    taurilua_app_t *app = taurilua_create_from_string(script.c_str(), nullptr);
    if (!app) return -1;
    int res = taurilua_run(app);
    taurilua_destroy(app);
    return res;
}

}
