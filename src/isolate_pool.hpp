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
#include <lua.hpp>

struct IPCTask {
    std::string label;
    std::string seq;
    std::string req;
};

class IsolatePool {
public:
    IsolatePool(size_t num_workers = 0) : stop_(false) {
        if (num_workers == 0) {
            num_workers = std::max(2u, std::thread::hardware_concurrency());
        }
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

    void enqueue(const std::string &label, const std::string &seq, const std::string &req) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            tasks_.push(IPCTask{label, seq, req});
        }
        cv_.notify_one();
    }

private:
    std::vector<std::thread> workers_;
    std::queue<IPCTask> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stop_;

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

    lua_State *create_worker_state(size_t id) {
        lua_State *L = luaL_newstate();
        if (!L) return nullptr;

        luaL_openlibs(L);

        lua_getglobal(L, "package");
        lua_getfield(L, -1, "preload");
        lua_pushcfunction(L, preload_tauri);
        lua_setfield(L, -2, "tauri");
        lua_pop(L, 2);

        std::string config_content = resolve_config_json();
        lua_pushlstring(L, config_content.data(), config_content.size());
        lua_setglobal(L, "__EMBEDDED_CONFIG_JSON__");

        lua_pushboolean(L, 1);
        lua_setglobal(L, "__IS_ISOLATE_WORKER__");

        lua_pushinteger(L, id);
        lua_setglobal(L, "__ISOLATE_WORKER_ID__");

        std::string main_path = resolve_main_lua();
        bool dev_mode = !main_path.empty();
        lua_pushboolean(L, dev_mode);
        lua_setglobal(L, "__TAURI_DEV__");

        int load_res = dev_mode ? luaL_loadfile(L, main_path.c_str())
                                : luaL_loadbuffer(L, (const char *)luaJIT_BC_main, sizeof(luaJIT_BC_main), "main");

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
                lua_pushstring(L, task.label.c_str());
                lua_pushstring(L, task.seq.c_str());
                lua_pushstring(L, task.req.c_str());
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