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

class IsolatePool {
public:
    static constexpr size_t MAX_QUEUE_SIZE = 1024;
    static constexpr size_t MAX_REQ_SIZE = 10 * 1024 * 1024; // 10MB cap

    IsolatePool(size_t num_workers = 0) : stop_(false) {
        if (num_workers == 0) {
            const char *env_w = getenv("TAURILUA_WORKERS");
            if (env_w) {
                num_workers = (size_t)atoi(env_w);
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
            if (tasks_.size() >= MAX_QUEUE_SIZE) {
                std::cerr << "[IsolatePool] IPC queue is full (" << tasks_.size() << " tasks); dropping request" << std::endl;
                return false;
            }
            tasks_.push(IPCTask{label, seq, req});
        }
        cv_.notify_one();
        return true;
    }

private:
    std::vector<std::thread> workers_;
    std::queue<IPCTask> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stop_;

    static int preload_tauri(lua_State *L) {
        bool dev_mode = ResourceResolver::is_dev_mode();
        if (dev_mode) {
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

    lua_State *create_worker_state(size_t id) {
        lua_State *L = luaL_newstate();
        if (!L) return nullptr;

        luaL_openlibs(L);

        lua_getglobal(L, "package");
        lua_getfield(L, -1, "preload");
        lua_pushcfunction(L, preload_tauri);
        lua_setfield(L, -2, "tauri");
        lua_pop(L, 2);

        bool dev_mode = ResourceResolver::is_dev_mode();

        std::string config_content = ResourceResolver::resolve_config_json(dev_mode, tauri_conf_json, tauri_conf_json_len);
        lua_pushlstring(L, config_content.data(), config_content.size());
        lua_setglobal(L, "__EMBEDDED_CONFIG_JSON__");

        lua_pushboolean(L, 1);
        lua_setglobal(L, "__IS_ISOLATE_WORKER__");

        lua_pushinteger(L, id);
        lua_setglobal(L, "__ISOLATE_WORKER_ID__");

        lua_pushboolean(L, dev_mode ? 1 : 0);
        lua_setglobal(L, "__TAURI_DEV__");

        std::string main_path = ResourceResolver::resolve_main_lua(dev_mode);
        int load_res = 0;
        if (dev_mode && !main_path.empty()) {
            load_res = luaL_loadfile(L, main_path.c_str());
        } else {
            load_res = luaL_loadbuffer(L, (const char *)luaJIT_BC_main, sizeof(luaJIT_BC_main), "main");
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