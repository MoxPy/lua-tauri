#ifndef ISOLATE_POOL_HPP
#define ISOLATE_POOL_HPP

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <string>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <functional>
#include <lua.hpp>

#include "resource_resolver.hpp"
#include "tauri_bc.h"
#include "main_bc.h"
#include "config_json.h"

struct IPCTask {
    std::string label;
    std::string seq;
    std::string req;
};

struct IsolateConfig {
    std::string config_json;
    std::string script_path;
    std::string inline_script;
    std::string startup_payload = "{\"args\":[]}";
    bool dev_mode = false;
    lua_CFunction host_cmd_dispatcher = nullptr;
    void *host_cmd_userdata = nullptr;
};

class IsolatePool {
public:
    static constexpr size_t MAX_QUEUE_SIZE = 1024;
    static constexpr size_t MAX_REQ_SIZE = 10 * 1024 * 1024;

    IsolatePool(const IsolateConfig &cfg = IsolateConfig(), size_t num_workers = 0)
        : cfg_(cfg), stop_(false) {
        if (num_workers == 0) {
            const char *env_w = getenv("TAURILUA_WORKERS");
            if (env_w) {
                num_workers = static_cast<size_t>(std::max(0, atoi(env_w)));
            } else {
                num_workers = 2;
            }
        }
        if (num_workers < 1) num_workers = 1;
        if (num_workers > 64) num_workers = 64;

        for (size_t i = 0; i < num_workers; ++i) {
            workers_.emplace_back(&IsolatePool::worker_loop, this, i);
        }
    }

    ~IsolatePool() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto &t : workers_) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    bool enqueue(const std::string &label, const std::string &seq, const std::string &req) {
        if (req.size() > MAX_REQ_SIZE) {
            std::cerr << "[IsolatePool] IPC request rejected: exceeds maximum payload size limit (10MB)" << std::endl;
            return false;
        }
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stop_ || tasks_.size() >= MAX_QUEUE_SIZE) {
                return false;
            }
            tasks_.push(IPCTask{label, seq, req});
        }
        cv_.notify_one();
        return true;
    }

private:
    IsolateConfig cfg_;
    std::vector<std::thread> workers_;
    std::queue<IPCTask> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stop_;

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

    lua_State *create_worker_state(size_t id) {
        lua_State *L = luaL_newstate();
        if (!L) return nullptr;

        luaL_openlibs(L);

        lua_getglobal(L, "package");
        lua_getfield(L, -1, "preload");
        lua_pushcfunction(L, preload_tauri);
        lua_setfield(L, -2, "tauri");
        lua_pop(L, 2);

#if defined(TAURILUA_RELEASE_FUSE)
        bool dev_mode = false;
#else
        bool dev_mode = cfg_.dev_mode;
#endif

        std::string config_content = !cfg_.config_json.empty()
            ? cfg_.config_json
            : ResourceResolver::resolve_config_json(dev_mode, tauri_conf_json, tauri_conf_json_len);
        lua_pushlstring(L, config_content.data(), config_content.size());
        lua_setglobal(L, "__EMBEDDED_CONFIG_JSON__");

        lua_pushlstring(L, cfg_.startup_payload.data(), cfg_.startup_payload.size());
        lua_setglobal(L, "__TAURI_STARTUP_PAYLOAD__");

        lua_pushboolean(L, 1);
        lua_setglobal(L, "__IS_ISOLATE_WORKER__");

        lua_pushinteger(L, static_cast<lua_Integer>(id));
        lua_setglobal(L, "__ISOLATE_WORKER_ID__");

        lua_pushboolean(L, dev_mode ? 1 : 0);
        lua_setglobal(L, "__TAURI_DEV__");

        if (cfg_.host_cmd_dispatcher) {
            lua_pushlightuserdata(L, cfg_.host_cmd_userdata);
            lua_pushcclosure(L, cfg_.host_cmd_dispatcher, 1);
            lua_setglobal(L, "__TAURI_INVOKE_HOST_CMD__");
        }

        int load_res = 0;
        if (!cfg_.inline_script.empty()) {
            load_res = luaL_loadbuffer(L, cfg_.inline_script.data(), cfg_.inline_script.size(), "host_inline");
        } else if (!cfg_.script_path.empty() && ResourceResolver::file_exists(cfg_.script_path)) {
            load_res = luaL_loadfile(L, cfg_.script_path.c_str());
        } else {
            std::string main_path = ResourceResolver::resolve_main_lua(dev_mode);
            if (!main_path.empty()) {
                load_res = luaL_loadfile(L, main_path.c_str());
            } else {
                load_res = luaL_loadbuffer(L, reinterpret_cast<const char *>(luaJIT_BC_main), sizeof(luaJIT_BC_main), "main");
            }
        }

        if (load_res != 0) {
            std::cerr << lua_tostring(L, -1) << std::endl;
            lua_close(L);
            return nullptr;
        }

        if (lua_pcall(L, 0, 0, 0) != 0) {
            std::cerr << lua_tostring(L, -1) << std::endl;
            lua_close(L);
            return nullptr;
        }

        return L;
    }

    void worker_loop(size_t id) {
        if (stop_) return;
        lua_State *L = create_worker_state(id);
        if (!L) return;

        while (true) {
            IPCTask task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] {
                    return stop_ || !tasks_.empty();
                });
                if (stop_ && tasks_.empty()) {
                    break;
                }
                task = std::move(tasks_.front());
                tasks_.pop();
            }

            lua_getglobal(L, "__TAURI_DISPATCH_IPC__");
            if (lua_isfunction(L, -1)) {
                lua_pushlstring(L, task.label.data(), task.label.size());
                lua_pushlstring(L, task.seq.data(), task.seq.size());
                lua_pushlstring(L, task.req.data(), task.req.size());
                if (lua_pcall(L, 3, 0, 0) != 0) {
                    std::cerr << lua_tostring(L, -1) << std::endl;
                    lua_pop(L, 1);
                }
            } else {
                lua_pop(L, 1);
            }
        }

        lua_close(L);
    }
};

#endif
