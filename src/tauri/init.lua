local ffi = require("ffi")
local bit = bit or require("bit")

local function filter_absolute_only(path_str)
    local parts = {}
    for part in (path_str .. ";"):gmatch("([^;]*);") do
        local clean = part:match("^%s*(.-)%s*$")
        if clean and clean ~= "" then
            local is_absolute = false
            if ffi.os == "Windows" then
                is_absolute = clean:match("^[a-zA-Z]:[/\\]") ~= nil or clean:match("^\\\\") ~= nil
            else
                is_absolute = clean:match("^/") ~= nil
            end
            if is_absolute then
                table.insert(parts, clean)
            end
        end
    end
    return table.concat(parts, ";")
end

-- ============================================================================
-- Module & Plugin Search Paths (Auto-Discovery)
-- In RELEASE builds only absolute exec-relative paths are added.
-- Relative cwd paths (./  ../) are restricted to DEV mode only, activated
-- by setting the TAURILUA_DEV=1 environment variable (set by `lua-tauri dev`).
-- ============================================================================
local function slugify_package_name(str)
    if not str or type(str) ~= "string" or str == "" then return "" end
    local res = str:gsub("%s+", "-"):gsub("[^%w%+%-%._]", ""):lower():gsub("^%-+", ""):gsub("%-+$", "")
    return res
end

local function get_executable_info()
    local dir = nil
    local exe_name = nil
    pcall(function()
        if ffi.os == "OSX" then
            ffi.cdef[[
                int _NSGetExecutablePath(char* buf, uint32_t* bufsize);
            ]]
            local buf = ffi.new("char[1024]")
            local size = ffi.new("uint32_t[1]", 1024)
            if ffi.C._NSGetExecutablePath(buf, size) == 0 then
                local full_path = ffi.string(buf)
                dir = full_path:match("^(.*)/[^/]+$")
                exe_name = full_path:match("^.*/([^/]+)$")
            end
        elseif ffi.os == "Linux" then
            ffi.cdef[[
                ssize_t readlink(const char *path, char *buf, size_t bufsiz);
            ]]
            local buf = ffi.new("char[1024]")
            local len = ffi.C.readlink("/proc/self/exe", buf, 1024)
            if len > 0 then
                local full_path = ffi.string(buf, len)
                dir = full_path:match("^(.*)/[^/]+$")
                exe_name = full_path:match("^.*/([^/]+)$")
            end
        elseif ffi.os == "Windows" then
            ffi.cdef[[
                uint32_t GetModuleFileNameW(void* hModule, uint16_t* lpFilename, uint32_t nSize);
                int WideCharToMultiByte(uint32_t CodePage, uint32_t dwFlags, const uint16_t* lpWideCharStr, int cchWideChar, char* lpMultiByteStr, int cbMultiByte, const char* lpDefaultChar, int* lpUsedDefaultChar);
            ]]
            local k32 = nil
            pcall(function() k32 = ffi.load("kernel32") end)
            local fn = (k32 and k32.GetModuleFileNameW) or ffi.C.GetModuleFileNameW
            local wbuf = ffi.new("uint16_t[1024]")
            local wlen = fn(nil, wbuf, 1024)
            if wlen > 0 then
                local wctomb = (k32 and k32.WideCharToMultiByte) or ffi.C.WideCharToMultiByte
                local ulen = wctomb(65001, 0, wbuf, wlen, nil, 0, nil, nil)
                if ulen > 0 then
                    local ubuf = ffi.new("char[?]", ulen)
                    wctomb(65001, 0, wbuf, wlen, ubuf, ulen, nil, nil)
                    local full_path = ffi.string(ubuf, ulen):gsub(string.char(92), "/")
                    dir = full_path:match("^(.*)/[^/]+$")
                    exe_name = full_path:match("^.*/([^/]+)$")
                    if exe_name then exe_name = exe_name:gsub("%.exe$", "") end
                end
            end
        end
    end)
    return dir, exe_name
end

local function get_executable_dir()
    local dir = get_executable_info()
    return dir
end

local function setup_module_search_paths(extra_conf)
    local function is_dev_mode()
        local ok, val = pcall(os.getenv, "TAURILUA_DEV")
        if ok and val and val ~= "" and val ~= "0" then
            return true
        end
        return false
    end

    local dev_mode = is_dev_mode()
    local exec_dir, exe_name = get_executable_info()

    local extra_lua_paths = {}
    local extra_c_paths = {}

    -- Candidate names for system share directories (e.g. /usr/share/<name>/...)
    local candidate_set = {}
    local candidates = {}
    local function add_candidate(val)
        if type(val) == "string" and val ~= "" then
            if not candidate_set[val] then
                candidate_set[val] = true
                table.insert(candidates, val)
            end
            local slug = slugify_package_name(val)
            if slug ~= "" and not candidate_set[slug] then
                candidate_set[slug] = true
                table.insert(candidates, slug)
            end
        end
    end

    if exe_name then add_candidate(exe_name) end

    local embedded_json = rawget(_G, "__EMBEDDED_CONFIG_JSON__")
    if type(embedded_json) == "string" and embedded_json ~= "" then
        add_candidate(embedded_json:match('"productName"%s*:%s*"([^"]+)"'))
        add_candidate(embedded_json:match('"product_name"%s*:%s*"([^"]+)"'))
        add_candidate(embedded_json:match('"package"%s*:%s*{.-"name"%s*:%s*"([^"]+)"'))
        add_candidate(embedded_json:match('"name"%s*:%s*"([^"]+)"'))
    end

    if type(extra_conf) == "table" then
        add_candidate(extra_conf.productName)
        add_candidate(extra_conf.product_name)
        if type(extra_conf.package) == "table" then
            add_candidate(extra_conf.package.productName)
            add_candidate(extra_conf.package.name)
        end
        add_candidate(extra_conf.name)
    end

    add_candidate("app")
    add_candidate("tauri-app")

    -- -----------------------------------------------------------------------
    -- DEV-ONLY relative paths (cwd-relative, sibling repos, node_modules).
    -- These are NOT included in production releases to prevent module hijacking
    -- via a malicious cwd or PATH-adjacent directory.
    -- -----------------------------------------------------------------------
    if dev_mode then
        local dev_lua_paths = {
            "./?.lua",
            "./?/init.lua",
            "./src-tauri/?.lua",
            "./src-tauri/?/init.lua",
            "./src-tauri/plugins/?.lua",
            "./src-tauri/plugins/?/init.lua",
            "./src-tauri/plugins/?/src/?.lua",
            "./plugins/?.lua",
            "./plugins/?/init.lua",
            "./plugins/?/src/?.lua",
            "../lua-tauri-plugins/plugins/?.lua",
            "../lua-tauri-plugins/plugins/?/init.lua",
            "../lua-tauri-plugins/plugins/?/src/?.lua",
            "../plugins/?.lua",
            "../plugins/?/init.lua",
            "./node_modules/?/init.lua",
            "./node_modules/@tauri-apps/plugin-?/init.lua",
            "./node_modules/lua-tauri-plugin-?/init.lua",
            -- Local hermetic LuaRocks tree (development)
            "./src-tauri/.rocks/share/lua/5.1/?.lua",
            "./src-tauri/.rocks/share/lua/5.1/?/init.lua",
            "./.rocks/share/lua/5.1/?.lua",
            "./.rocks/share/lua/5.1/?/init.lua",
            "../src-tauri/.rocks/share/lua/5.1/?.lua",
            "../src-tauri/.rocks/share/lua/5.1/?/init.lua",
            "../.rocks/share/lua/5.1/?.lua",
            "../.rocks/share/lua/5.1/?/init.lua"
        }
        local dev_c_paths = {
            "./?.so",
            "./?.dylib",
            "./?.dll",
            "./plugins/?.so",
            "./plugins/?.dylib",
            "./plugins/?.dll",
            "../lua-tauri-plugins/plugins/?/?.so",
            "../lua-tauri-plugins/plugins/?/?.dylib",
            "../lua-tauri-plugins/plugins/?/?.dll",
            -- Local hermetic LuaRocks C modules (development)
            "./src-tauri/.rocks/lib/lua/5.1/?.so",
            "./src-tauri/.rocks/lib/lua/5.1/?.dylib",
            "./src-tauri/.rocks/lib/lua/5.1/?.dll",
            "./.rocks/lib/lua/5.1/?.so",
            "./.rocks/lib/lua/5.1/?.dylib",
            "./.rocks/lib/lua/5.1/?.dll",
            "../src-tauri/.rocks/lib/lua/5.1/?.so",
            "../src-tauri/.rocks/lib/lua/5.1/?.dylib",
            "../src-tauri/.rocks/lib/lua/5.1/?.dll",
            "../.rocks/lib/lua/5.1/?.so",
            "../.rocks/lib/lua/5.1/?.dylib",
            "../.rocks/lib/lua/5.1/?.dll"
        }
        for _, p in ipairs(dev_lua_paths) do table.insert(extra_lua_paths, p) end
        for _, p in ipairs(dev_c_paths)   do table.insert(extra_c_paths, p)   end
    end

    -- -----------------------------------------------------------------------
    -- PRODUCTION absolute paths (exec-relative bundle layout).
    -- Safe in both dev and release; always included.
    -- -----------------------------------------------------------------------
    if exec_dir then
        if exec_dir:match("/Contents/MacOS$") then
            -- macOS App Bundle (.app/Contents/Resources)
            local res_dir = exec_dir:gsub("/MacOS$", "/Resources")
            if dev_mode then
                table.insert(extra_lua_paths, res_dir .. "/?.lua")
                table.insert(extra_lua_paths, res_dir .. "/?/init.lua")
                table.insert(extra_lua_paths, res_dir .. "/src/?.lua")
                table.insert(extra_lua_paths, res_dir .. "/src/?/init.lua")
                table.insert(extra_lua_paths, res_dir .. "/src-tauri/src/?.lua")
                table.insert(extra_lua_paths, res_dir .. "/src-tauri/src/?/init.lua")
            end
            table.insert(extra_lua_paths, res_dir .. "/plugins/?.lua")
            table.insert(extra_lua_paths, res_dir .. "/plugins/?/init.lua")
            table.insert(extra_lua_paths, res_dir .. "/plugins/?/src/?.lua")
            table.insert(extra_lua_paths, res_dir .. "/src-tauri/plugins/?.lua")
            table.insert(extra_lua_paths, res_dir .. "/src-tauri/plugins/?/init.lua")
            table.insert(extra_lua_paths, res_dir .. "/rocks/share/lua/5.1/?.lua")
            table.insert(extra_lua_paths, res_dir .. "/rocks/share/lua/5.1/?/init.lua")
            table.insert(extra_c_paths,   res_dir .. "/rocks/lib/lua/5.1/?.so")
            table.insert(extra_c_paths,   res_dir .. "/rocks/lib/lua/5.1/?.dylib")
        else
            -- Linux / Windows standalone or installed packages
            -- 1. Exec-relative standalone paths
            table.insert(extra_lua_paths, exec_dir .. "/plugins/?.lua")
            table.insert(extra_lua_paths, exec_dir .. "/plugins/?/init.lua")
            table.insert(extra_lua_paths, exec_dir .. "/plugins/?/src/?.lua")
            if dev_mode then
                table.insert(extra_lua_paths, exec_dir .. "/src/?.lua")
                table.insert(extra_lua_paths, exec_dir .. "/src/?/init.lua")
                table.insert(extra_lua_paths, exec_dir .. "/?.lua")
                table.insert(extra_lua_paths, exec_dir .. "/?/init.lua")
            end
            table.insert(extra_lua_paths, exec_dir .. "/../share/plugins/?.lua")
            table.insert(extra_lua_paths, exec_dir .. "/../share/plugins/?/init.lua")
            table.insert(extra_lua_paths, exec_dir .. "/rocks/share/lua/5.1/?.lua")
            table.insert(extra_lua_paths, exec_dir .. "/rocks/share/lua/5.1/?/init.lua")
            table.insert(extra_lua_paths, exec_dir .. "/../Resources/rocks/share/lua/5.1/?.lua")
            table.insert(extra_lua_paths, exec_dir .. "/../Resources/rocks/share/lua/5.1/?/init.lua")
            table.insert(extra_lua_paths, exec_dir .. "/../share/rocks/share/lua/5.1/?.lua")
            table.insert(extra_lua_paths, exec_dir .. "/../share/rocks/share/lua/5.1/?/init.lua")
            table.insert(extra_c_paths,   exec_dir .. "/rocks/lib/lua/5.1/?.so")
            table.insert(extra_c_paths,   exec_dir .. "/rocks/lib/lua/5.1/?.dylib")
            table.insert(extra_c_paths,   exec_dir .. "/rocks/lib/lua/5.1/?.dll")
            table.insert(extra_c_paths,   exec_dir .. "/../Resources/rocks/lib/lua/5.1/?.so")
            table.insert(extra_c_paths,   exec_dir .. "/../Resources/rocks/lib/lua/5.1/?.dylib")
            table.insert(extra_c_paths,   exec_dir .. "/../share/rocks/lib/lua/5.1/?.so")

            -- 2. Linux system packages (/usr/share/<name> and /usr/local/share/<name>)
            for _, c in ipairs(candidates) do
                table.insert(extra_lua_paths, "/usr/share/" .. c .. "/plugins/?.lua")
                table.insert(extra_lua_paths, "/usr/share/" .. c .. "/plugins/?/init.lua")
                table.insert(extra_lua_paths, "/usr/share/" .. c .. "/plugins/?/src/?.lua")
                if dev_mode then
                    table.insert(extra_lua_paths, "/usr/share/" .. c .. "/src/?.lua")
                    table.insert(extra_lua_paths, "/usr/share/" .. c .. "/src/?/init.lua")
                    table.insert(extra_lua_paths, "/usr/share/" .. c .. "/?.lua")
                    table.insert(extra_lua_paths, "/usr/share/" .. c .. "/?/init.lua")
                end
                table.insert(extra_lua_paths, "/usr/share/" .. c .. "/rocks/share/lua/5.1/?.lua")
                table.insert(extra_lua_paths, "/usr/share/" .. c .. "/rocks/share/lua/5.1/?/init.lua")

                table.insert(extra_lua_paths, "/usr/local/share/" .. c .. "/plugins/?.lua")
                table.insert(extra_lua_paths, "/usr/local/share/" .. c .. "/plugins/?/init.lua")
                table.insert(extra_lua_paths, "/usr/local/share/" .. c .. "/plugins/?/src/?.lua")
                if dev_mode then
                    table.insert(extra_lua_paths, "/usr/local/share/" .. c .. "/src/?.lua")
                    table.insert(extra_lua_paths, "/usr/local/share/" .. c .. "/src/?/init.lua")
                    table.insert(extra_lua_paths, "/usr/local/share/" .. c .. "/?.lua")
                    table.insert(extra_lua_paths, "/usr/local/share/" .. c .. "/?/init.lua")
                end
                table.insert(extra_lua_paths, "/usr/local/share/" .. c .. "/rocks/share/lua/5.1/?.lua")
                table.insert(extra_lua_paths, "/usr/local/share/" .. c .. "/rocks/share/lua/5.1/?/init.lua")

                table.insert(extra_lua_paths, exec_dir .. "/../share/" .. c .. "/plugins/?.lua")
                table.insert(extra_lua_paths, exec_dir .. "/../share/" .. c .. "/plugins/?/init.lua")
                table.insert(extra_lua_paths, exec_dir .. "/../share/" .. c .. "/plugins/?/src/?.lua")
                if dev_mode then
                    table.insert(extra_lua_paths, exec_dir .. "/../share/" .. c .. "/src/?.lua")
                    table.insert(extra_lua_paths, exec_dir .. "/../share/" .. c .. "/src/?/init.lua")
                    table.insert(extra_lua_paths, exec_dir .. "/../share/" .. c .. "/?.lua")
                    table.insert(extra_lua_paths, exec_dir .. "/../share/" .. c .. "/?/init.lua")
                end
                table.insert(extra_lua_paths, exec_dir .. "/../share/" .. c .. "/rocks/share/lua/5.1/?.lua")
                table.insert(extra_lua_paths, exec_dir .. "/../share/" .. c .. "/rocks/share/lua/5.1/?/init.lua")

                table.insert(extra_c_paths, "/usr/share/" .. c .. "/rocks/lib/lua/5.1/?.so")
                table.insert(extra_c_paths, "/usr/local/share/" .. c .. "/rocks/lib/lua/5.1/?.so")
                table.insert(extra_c_paths, exec_dir .. "/../share/" .. c .. "/rocks/lib/lua/5.1/?.so")
                table.insert(extra_c_paths, "/usr/lib/" .. c .. "/?.so")
            end
        end
    end

    if dev_mode then
        local current_path = package.path or ""
        for _, p in ipairs(extra_lua_paths) do
            if not current_path:find(p, 1, true) then
                current_path = current_path .. ";" .. p
            end
        end
        package.path = current_path

        local current_cpath = package.cpath or ""
        for _, cp in ipairs(extra_c_paths) do
            if not current_cpath:find(cp, 1, true) then
                current_cpath = current_cpath .. ";" .. cp
            end
        end
        package.cpath = current_cpath
    else
        package.path = filter_absolute_only(table.concat(extra_lua_paths, ";"))
        package.cpath = filter_absolute_only(table.concat(extra_c_paths, ";"))
    end
end

setup_module_search_paths()

pcall(ffi.cdef, [[
typedef void (*ipc_callback_fn)(const char *window_label, const char *seq, const char *req);
typedef void (*tray_callback_fn)(int id);
typedef void (*shortcut_callback_fn)(const char *shortcut);
typedef void (*deeplink_callback_fn)(const char *url);

void bridge_init(ipc_callback_fn on_ipc, tray_callback_fn on_tray, shortcut_callback_fn on_shortcut, deeplink_callback_fn on_deeplink);
void bridge_run_loop();
void bridge_terminate();
void bridge_set_dev_mode(int dev);
void bridge_set_dev_url(const char *url);

int platform_list_dir(const char *dir_path, char *out_buf, int max_len);
void platform_set_assets_path(const char *path);
int platform_is_origin_allowed(const char *url);
int platform_resolve_path(const char *in_path, char *out_path, int max_len);

int platform_window_create(const char *label, const char *title, const char *url, int width, int height, int resizable, int decorations, int transparent);
void platform_window_close(const char *label);
void platform_window_force_close(const char *label);
void platform_window_set_title(const char *label, const char *title);
void platform_window_set_size(const char *label, int width, int height);
void platform_window_minimize(const char *label);
void platform_window_maximize(const char *label);
void platform_window_unmaximize(const char *label);
void platform_window_show(const char *label);
void platform_window_hide(const char *label);
void platform_window_set_focus(const char *label);
int platform_single_instance_init(const char *app_id, const char *payload_json);
void platform_single_instance_cleanup();
void platform_window_start_dragging(const char *label);
void platform_set_client_script(const char *js);
void platform_window_init_script(const char *label, const char *js);
void platform_window_eval(const char *label, const char *js);
void platform_window_return_async(const char *label, const char *seq, int status, const char *result);
void platform_register_child_pid(long pid);
void platform_unregister_child_pid(long pid);
void platform_kill_child_processes();

int platform_tray_create(const char *tooltip, const char *icon_path);
void platform_tray_set_menu(const char *json_menu);
void platform_tray_destroy();
void platform_set_menu(const char *json_menu);
void platform_set_app_name(const char *name);
const char *platform_get_app_name();

int platform_shortcut_register(const char *shortcut);
void platform_shortcut_unregister(const char *shortcut);

int platform_deeplink_register(const char *scheme);
int platform_updater_install(const char *archive_path);

int platform_dialog_open(const char *title, const char *filter, char *out_path, int max_len);
int platform_dialog_save(const char *title, const char *filter, char *out_path, int max_len);
int platform_dialog_message(const char *title, const char *message, int is_question);
int platform_clipboard_write(const char *text);
int platform_clipboard_read(char *out_text, int max_len);
int platform_clipboard_write_html(const char *html);
int platform_clipboard_read_html(char *out_html, int max_len);
int platform_clipboard_write_image(const char *base64_png);
int platform_clipboard_read_image(char *out_base64_png, int max_len);
void platform_clipboard_clear();
const char *platform_get_theme();
void platform_notification_send(const char *title, const char *body);

void platform_state_set_string(const char *key, const char *val);
int platform_state_get_string(const char *key, char *out_val, int max_len);
void platform_state_set_number(const char *key, double val);
int platform_state_get_number(const char *key, double *out_val);
void platform_state_set_json(const char *key, const char *json_val);
int platform_state_get_json(const char *key, char *out_val, int max_len);
int platform_state_get_length(const char *key);
int platform_state_has(const char *key);
void platform_state_delete(const char *key);
void platform_state_clear();
void platform_state_lock();
void platform_state_unlock();

void platform_set_csp(const char *csp);
void *platform_process_spawn(const char *program, const char **args, int arg_count);
int platform_process_read_line(void *handle, char *out_buf, int max_len);
int platform_process_wait(void *handle);
void platform_process_close(void *handle);

int mkdir(const char *pathname, unsigned int mode);
int _mkdir(const char *pathname);

typedef struct _FILETIME {
    unsigned long dwLowDateTime;
    unsigned long dwHighDateTime;
} FILETIME;

typedef struct _WIN32_FIND_DATAW {
    unsigned long dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    unsigned long nFileSizeHigh;
    unsigned long nFileSizeLow;
    unsigned long dwReserved0;
    unsigned long dwReserved1;
    uint16_t cFileName[260];
    uint16_t cAlternateFileName[14];
} WIN32_FIND_DATAW;

void* FindFirstFileW(const uint16_t *lpFileName, WIN32_FIND_DATAW *lpFindFileData);
int FindNextFileW(void *hFindFile, WIN32_FIND_DATAW *lpFindFileData);
int FindClose(void *hFindFile);
int MultiByteToWideChar(uint32_t CodePage, uint32_t dwFlags, const char* lpMultiByteStr, int cbMultiByte, uint16_t* lpWideCharStr, int cchWideChar);
int WideCharToMultiByte(uint32_t CodePage, uint32_t dwFlags, const uint16_t* lpWideCharStr, int cchWideChar, char* lpMultiByteStr, int cbMultiByte, const char* lpDefaultChar, int* lpUsedDefaultChar);

typedef struct DIR DIR;
typedef long off_t;
struct dirent {
    unsigned long d_ino;
    off_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[256];
};
DIR *opendir(const char *name);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);
int remove(const char *pathname);
int usleep(unsigned int usec);
void Sleep(unsigned long dwMilliseconds);
char *realpath(const char *path, char *resolved_path);
]])

local clib = ffi.C
local has_sym = pcall(function() return ffi.C.platform_state_has end)
if not has_sym then
    pcall(function()
        local candidates = {
            "libtaurilua",
            "./build/libtaurilua.dylib",
            "./build/libtaurilua.so",
            "./build/Release/taurilua.dll",
            "./build/taurilua.dll",
            "taurilua"
        }
        for _, name in ipairs(candidates) do
            local ok, lib = pcall(ffi.load, name)
            if ok and lib then
                clib = lib
                break
            end
        end
    end)
end

local has_state_sym = pcall(function() return clib.platform_state_has end)
if not has_state_sym then
    local _state_store = {}
    local _state_types = {}
    local real_clib = clib
    clib = setmetatable({
        platform_state_lock = function() end,
        platform_state_unlock = function() end,
        platform_state_has = function(k)
            local sk = type(k) == "string" and k or ffi.string(k)
            return _state_store[sk] ~= nil and 1 or 0
        end,
        platform_state_delete = function(k)
            local sk = type(k) == "string" and k or ffi.string(k)
            _state_store[sk] = nil
            _state_types[sk] = nil
        end,
        platform_state_set_number = function(k, v)
            local sk = type(k) == "string" and k or ffi.string(k)
            _state_store[sk] = tonumber(v)
            _state_types[sk] = "number"
        end,
        platform_state_set_string = function(k, v)
            local sk = type(k) == "string" and k or ffi.string(k)
            _state_store[sk] = type(v) == "string" and v or ffi.string(v)
            _state_types[sk] = "string"
        end,
        platform_state_set_json = function(k, v)
            local sk = type(k) == "string" and k or ffi.string(k)
            _state_store[sk] = type(v) == "string" and v or ffi.string(v)
            _state_types[sk] = "json"
        end,
        platform_state_get_number = function(k, buf)
            local sk = type(k) == "string" and k or ffi.string(k)
            if _state_types[sk] == "number" then
                buf[0] = _state_store[sk]
                return 1
            end
            return 0
        end,
        platform_state_get_string = function(k, buf, maxlen)
            local sk = type(k) == "string" and k or ffi.string(k)
            if _state_types[sk] == "string" then
                local s = _state_store[sk]
                local len = #s
                if not buf or maxlen <= 0 or len >= maxlen then
                    if buf and maxlen > 0 then
                        ffi.copy(buf, s, maxlen - 1)
                        buf[maxlen - 1] = 0
                    end
                    return len + 1
                end
                ffi.copy(buf, s, len + 1)
                return 1
            end
            return 0
        end,
        platform_state_get_json = function(k, buf, maxlen)
            local sk = type(k) == "string" and k or ffi.string(k)
            if _state_types[sk] == "json" then
                local s = _state_store[sk]
                local len = #s
                if not buf or maxlen <= 0 or len >= maxlen then
                    if buf and maxlen > 0 then
                        ffi.copy(buf, s, maxlen - 1)
                        buf[maxlen - 1] = 0
                    end
                    return len + 1
                end
                ffi.copy(buf, s, len + 1)
                return 1
            end
            return 0
        end,
        platform_state_get_length = function(k)
            local sk = type(k) == "string" and k or ffi.string(k)
            if _state_types[sk] == "string" or _state_types[sk] == "json" then
                local s = _state_store[sk]
                return s and #s or 0
            end
            return 0
        end,
        platform_state_clear = function()
            _state_store = {}
            _state_types = {}
        end,
        platform_window_eval = function(...) end,
        platform_resolve_path = function(in_path, out_buf, maxlen)
            local p = type(in_path) == "string" and in_path or ffi.string(in_path)
            if not out_buf or maxlen <= 0 then return 0 end
            local ok, rp = pcall(function() return real_clib.realpath(p, out_buf) end)
            if ok and rp ~= nil then
                return 1
            end
            ffi.copy(out_buf, p, math.min(#p + 1, maxlen))
            return 1
        end,
    }, {
        __index = function(_, k)
            local ok, val = pcall(function() return real_clib[k] end)
            if ok then return val end
            return nil
        end
    })
end

local json = {}

local null_mt = {
    __tostring = function() return "null" end,
    __is_json_null = true
}
json.null = setmetatable({}, null_mt)

local array_mt = {
    __tostring = function() return "[]" end,
    __is_json_array = true
}
json.empty_array = setmetatable({}, array_mt)
json.array = function(t)
    return setmetatable(t or {}, array_mt)
end

local function utf8_encode(cp)
    if cp < 0x80 then
        return string.char(cp)
    elseif cp < 0x800 then
        return string.char(0xC0 + math.floor(cp / 0x40), 0x80 + (cp % 0x40))
    elseif cp < 0x10000 then
        return string.char(0xE0 + math.floor(cp / 0x1000), 0x80 + (math.floor(cp / 0x40) % 0x40), 0x80 + (cp % 0x40))
    else
        return string.char(
            0xF0 + math.floor(cp / 0x40000),
            0x80 + (math.floor(cp / 0x1000) % 0x40),
            0x80 + (math.floor(cp / 0x40) % 0x40),
            0x80 + (cp % 0x40)
        )
    end
end

local escape_map = {
    ['"'] = '\\"',
    ['\\'] = '\\\\',
    ['\b'] = '\\b',
    ['\f'] = '\\f',
    ['\n'] = '\\n',
    ['\r'] = '\\r',
    ['\t'] = '\\t'
}

local function encode_string(s)
    return '"' .. s:gsub('[%z\1-\31\\"]', function(c)
        return escape_map[c] or string.format('\\u%04x', string.byte(c))
    end) .. '"'
end

function json.encode(val, visited)
    visited = visited or {}
    local t = type(val)
    if val == nil or val == json.null or (t == "table" and getmetatable(val) and getmetatable(val).__is_json_null) then
        return "null"
    elseif t == "boolean" then
        return val and "true" or "false"
    elseif t == "number" then
        if val ~= val then
            error("JSON encode error: NaN is not allowed in RFC 8259")
        elseif val == math.huge or val == -math.huge then
            error("JSON encode error: Infinity is not allowed in RFC 8259")
        end
        if math.floor(val) == val and math.abs(val) < 1e15 then
            return string.format("%.0f", val)
        else
            return string.format("%.17g", val)
        end
    elseif t == "string" then
        return encode_string(val)
    elseif t == "table" then
        if visited[val] then
            error("JSON encode error: circular reference detected")
        end
        visited[val] = true

        local mt = getmetatable(val)
        local is_explicit_array = (mt and mt.__is_json_array == true) or (val == json.empty_array)

        local is_array = true
        local max_idx = 0
        local count = 0
        for k, _ in pairs(val) do
            count = count + 1
            if type(k) == "number" and k > 0 and math.floor(k) == k then
                if k > max_idx then max_idx = k end
            else
                is_array = false
            end
        end

        if count == 0 then
            visited[val] = nil
            if is_explicit_array then
                return "[]"
            end
            return "{}"
        end

        if (is_array and max_idx == count) or (is_explicit_array and is_array) then
            local parts = {}
            for i = 1, max_idx do
                parts[i] = json.encode(val[i], visited)
            end
            visited[val] = nil
            return "[" .. table.concat(parts, ",") .. "]"
        else
            local parts = {}
            for k, v in pairs(val) do
                table.insert(parts, encode_string(tostring(k)) .. ":" .. json.encode(v, visited))
            end
            visited[val] = nil
            return "{" .. table.concat(parts, ",") .. "}"
        end
    else
        error("JSON encode error: unsupported type " .. t)
    end
end

function json.decode(str)
    if type(str) ~= "string" or str == "" then
        return nil, "empty input"
    end

    local len = #str
    local pos = 1
    local depth = 0
    local max_depth = 128

    local function skip_ws()
        while pos <= len do
            local c = str:sub(pos, pos)
            if c == " " or c == "\t" or c == "\r" or c == "\n" then
                pos = pos + 1
            else
                break
            end
        end
    end

    local parse_value

    local function parse_string()
        pos = pos + 1
        local chunks = {}
        local start = pos
        while pos <= len do
            local c = str:sub(pos, pos)
            if c == '"' then
                if pos > start then
                    table.insert(chunks, str:sub(start, pos - 1))
                end
                pos = pos + 1
                return table.concat(chunks)
            elseif c == '\\' then
                if pos > start then
                    table.insert(chunks, str:sub(start, pos - 1))
                end
                pos = pos + 1
                if pos > len then return nil, "unterminated escape sequence" end
                local esc = str:sub(pos, pos)
                if esc == '"' or esc == '\\' or esc == '/' then
                    table.insert(chunks, esc)
                    pos = pos + 1
                elseif esc == 'b' then
                    table.insert(chunks, "\b")
                    pos = pos + 1
                elseif esc == 'f' then
                    table.insert(chunks, "\f")
                    pos = pos + 1
                elseif esc == 'n' then
                    table.insert(chunks, "\n")
                    pos = pos + 1
                elseif esc == 'r' then
                    table.insert(chunks, "\r")
                    pos = pos + 1
                elseif esc == 't' then
                    table.insert(chunks, "\t")
                    pos = pos + 1
                elseif esc == 'u' then
                    pos = pos + 1
                    if pos + 3 > len then return nil, "incomplete \\uXXXX escape" end
                    local hex = str:sub(pos, pos + 3)
                    if not hex:match("^[0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F]$") then
                        return nil, "invalid \\uXXXX escape: " .. hex
                    end
                    pos = pos + 4
                    local cp = tonumber(hex, 16)
                    if cp >= 0xD800 and cp <= 0xDBFF then
                        if str:sub(pos, pos + 1) == "\\u" then
                            local low_hex = str:sub(pos + 2, pos + 5)
                            if low_hex:match("^[0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F]$") then
                                local low = tonumber(low_hex, 16)
                                if low >= 0xDC00 and low <= 0xDFFF then
                                    pos = pos + 6
                                    cp = 0x10000 + ((cp - 0xD800) * 0x400) + (low - 0xDC00)
                                end
                            end
                        end
                    end
                    table.insert(chunks, utf8_encode(cp))
                else
                    return nil, "invalid escape char \\" .. esc
                end
                start = pos
            else
                local b = string.byte(c)
                if b < 32 then
                    return nil, "unescaped control character in string"
                end
                pos = pos + 1
            end
        end
        return nil, "unterminated string"
    end

    local function parse_object()
        pos = pos + 1
        depth = depth + 1
        if depth > max_depth then return nil, "maximum depth exceeded" end
        local obj = {}
        skip_ws()
        if pos <= len and str:sub(pos, pos) == '}' then
            pos = pos + 1
            depth = depth - 1
            return obj
        end

        while pos <= len do
            skip_ws()
            if pos > len then return nil, "unterminated object" end
            if str:sub(pos, pos) ~= '"' then
                return nil, "expected string key in object at position " .. pos
            end
            local key, err = parse_string()
            if not key then return nil, err end

            skip_ws()
            if pos > len or str:sub(pos, pos) ~= ':' then
                return nil, "expected ':' after key in object at position " .. pos
            end
            pos = pos + 1

            skip_ws()
            local val, val_err = parse_value()
            if val_err then return nil, val_err end
            obj[key] = val

            skip_ws()
            if pos <= len and str:sub(pos, pos) == '}' then
                pos = pos + 1
                depth = depth - 1
                return obj
            elseif pos <= len and str:sub(pos, pos) == ',' then
                pos = pos + 1
                skip_ws()
                if pos <= len and str:sub(pos, pos) == '}' then
                    return nil, "trailing comma in object"
                end
            else
                return nil, "expected ',' or '}' in object at position " .. pos
            end
        end
        return nil, "unterminated object"
    end

    local function parse_array()
        pos = pos + 1
        depth = depth + 1
        if depth > max_depth then return nil, "maximum depth exceeded" end
        local arr = setmetatable({}, array_mt)
        skip_ws()
        if pos <= len and str:sub(pos, pos) == ']' then
            pos = pos + 1
            depth = depth - 1
            return arr
        end

        local idx = 1
        while pos <= len do
            skip_ws()
            local val, err = parse_value()
            if err then return nil, err end
            arr[idx] = (val == nil) and json.null or val
            idx = idx + 1

            skip_ws()
            if pos <= len and str:sub(pos, pos) == ']' then
                pos = pos + 1
                depth = depth - 1
                return arr
            elseif pos <= len and str:sub(pos, pos) == ',' then
                pos = pos + 1
                skip_ws()
                if pos <= len and str:sub(pos, pos) == ']' then
                    return nil, "trailing comma in array"
                end
            else
                return nil, "expected ',' or ']' in array at position " .. pos
            end
        end
        return nil, "unterminated array"
    end

    local function parse_number()
        local start = pos
        if str:sub(pos, pos) == '-' then pos = pos + 1 end
        if pos > len then return nil, "invalid number" end
        if str:sub(pos, pos) == '0' then
            pos = pos + 1
        elseif str:sub(pos, pos):match("^[1-9]$") then
            while pos <= len and str:sub(pos, pos):match("^[0-9]$") do
                pos = pos + 1
            end
        else
            return nil, "invalid number format at position " .. pos
        end

        if pos <= len and str:sub(pos, pos) == '.' then
            pos = pos + 1
            if pos > len or not str:sub(pos, pos):match("^[0-9]$") then
                return nil, "invalid fractional number"
            end
            while pos <= len and str:sub(pos, pos):match("^[0-9]$") do
                pos = pos + 1
            end
        end

        if pos <= len and (str:sub(pos, pos) == 'e' or str:sub(pos, pos) == 'E') then
            pos = pos + 1
            if pos <= len and (str:sub(pos, pos) == '+' or str:sub(pos, pos) == '-') then
                pos = pos + 1
            end
            if pos > len or not str:sub(pos, pos):match("^[0-9]$") then
                return nil, "invalid exponent in number"
            end
            while pos <= len and str:sub(pos, pos):match("^[0-9]$") do
                pos = pos + 1
            end
        end

        local num_str = str:sub(start, pos - 1)
        local num = tonumber(num_str)
        if not num then return nil, "cannot convert number " .. num_str end
        return num
    end

    function parse_value()
        skip_ws()
        if pos > len then return nil, "unexpected end of input" end
        local c = str:sub(pos, pos)
        if c == '{' then
            return parse_object()
        elseif c == '[' then
            return parse_array()
        elseif c == '"' then
            return parse_string()
        elseif c == 't' then
            if str:sub(pos, pos + 3) == "true" then
                pos = pos + 4
                return true
            else
                return nil, "unexpected token at position " .. pos
            end
        elseif c == 'f' then
            if str:sub(pos, pos + 4) == "false" then
                pos = pos + 5
                return false
            else
                return nil, "unexpected token at position " .. pos
            end
        elseif c == 'n' then
            if str:sub(pos, pos + 3) == "null" then
                pos = pos + 4
                return nil
            else
                return nil, "unexpected token at position " .. pos
            end
        elseif c == '-' or c:match("^[0-9]$") then
            return parse_number()
        else
            return nil, "unexpected character '" .. c .. "' at position " .. pos
        end
    end

    local res, err = parse_value()
    if err then return nil, err end
    skip_ws()
    if pos <= len then
        return nil, "trailing characters after JSON at position " .. pos
    end
    return res
end

local Security = {}
Security.__index = Security

function Security.new(config)
    local self = setmetatable({}, Security)
    local sec = (config.app and config.app.security) or (config.tauri and config.tauri.security) or config.security or {}
    self.capabilities = {}
    if sec.capabilities then
        for _, cap in ipairs(sec.capabilities) do
            self.capabilities[cap] = true
        end
    end

    -- Support capability files:
    -- In release mode, NEVER scan CWD! Only scan bundle directory if provided.
    -- In dev mode, scan explicit dev capability dirs: src-tauri/capabilities and capabilities.
    local is_dev = rawget(_G, "__TAURI_DEV__")
    if is_dev == nil then
        local env_dev = os.getenv("TAURILUA_DEV")
        is_dev = (env_dev and env_dev ~= "" and env_dev ~= "0")
    end

    local cap_dirs = {}
    if is_dev then
        table.insert(cap_dirs, "src-tauri/capabilities")
        table.insert(cap_dirs, "capabilities")
    end
    local bundle_cap = rawget(_G, "__TAURI_BUNDLE_CAPABILITIES_DIR__")
    if bundle_cap and bundle_cap ~= "" then
        table.insert(cap_dirs, bundle_cap)
    end

    local seen_files = {}
    for _, cap_dir in ipairs(cap_dirs) do
        local buf_size = 65536
        local buf = ffi.new("char[?]", buf_size)
        local n = 0
        local has_list_dir, fn_list_dir = pcall(function() return clib and clib.platform_list_dir end)
        if has_list_dir and fn_list_dir then
            pcall(function() n = fn_list_dir(cap_dir, buf, buf_size) end)
        end
        if n > 0 then
            local file_list_str = ffi.string(buf, n)
            for fname in file_list_str:gmatch("[^\r\n]+") do
                fname = fname:match("^%s*(.-)%s*$")
                if fname:match("%.json$") and not seen_files[fname] then
                    seen_files[fname] = true
                    local full_path = cap_dir .. "/" .. fname
                    local f = io.open(full_path, "r")
                    if not f then
                        error(string.format("Capability discovery error: cannot open capability file '%s'", full_path))
                    end
                    local content = f:read("*a")
                    f:close()
                    local parsed, parse_err = json.decode(content)
                    if not parsed or type(parsed) ~= "table" then
                        error(string.format("Capability discovery error: malformed JSON in capability file '%s': %s", full_path, tostring(parse_err)))
                    end
                    local list = parsed.permissions or parsed.capabilities
                    if not list or type(list) ~= "table" then
                        error(string.format("Capability discovery error: missing permissions/capabilities array in '%s'", full_path))
                    end
                    for _, perm in ipairs(list) do
                        if type(perm) == "string" and perm ~= "" then
                            self.capabilities[perm] = true
                        else
                            error(string.format("Capability discovery error: invalid permission in '%s'", full_path))
                        end
                    end
                end
            end
        end
    end

    self.scopes = sec.scope or {}

    -- allowed_programs: list of strings or tables {name="cmd", args={"--allowed-flag", ...}}
    -- String entry means: program allowed (subject to dangerous flag blocking).
    -- Table entry means: program allowed ONLY with args from the given list.
    self.allowed_programs = sec.allowed_programs or sec.allowed_commands or {}
    -- Build a fast-lookup map: program_name -> arg_policy (false = explicit opt-in for any args, table = strict args)
    self._program_map = {}
    for _, entry in ipairs(self.allowed_programs) do
        if type(entry) == "string" then
            if entry == "*" then
                self._program_map["*"] = {}
            else
                self._program_map[entry] = {}
            end
        elseif type(entry) == "table" then
            local prog = entry.name or entry.program or entry.cmd
            if prog then
                if entry.any_args == true or entry.args == "*" or entry.args == false then
                    self._program_map[prog] = false  -- explicit opt-in for any arguments
                elseif type(entry.args) == "table" then
                    self._program_map[prog] = entry.args
                else
                    self._program_map[prog] = {}
                end
            end
        end
    end
    return self
end

function Security:assert_capability(name)
    if self.capabilities[name] or self.capabilities["tauri:" .. name] then
        return true
    end
    local stripped = name:gsub("^tauri:", "")
    if self.capabilities[stripped] then
        return true
    end
    local prefix = stripped:match("^([^:]+):")
    if prefix and (self.capabilities[prefix .. ":*"] or self.capabilities[prefix .. ":all"] or self.capabilities[prefix .. ":default"]) then
        if prefix ~= "fs" then
            if stripped == "window:create:external" and (self.capabilities["window:default"] or self.capabilities["core:default"]) then
            elseif (stripped == "state:set" or stripped == "state:write") and self.capabilities["state:default"] and not self.capabilities["state:*"] and not self.capabilities["state:all"] and not self.capabilities["state:set"] and not self.capabilities["state:write"] then
            else
                return true
            end
        end
    end
    if self.capabilities["fs:default"] and (stripped == "fs:read-text-file" or stripped == "fs:read-file" or stripped == "fs:read-dir" or stripped == "fs:exists" or stripped == "fs:stat") then
        return true
    end
    if self.capabilities["core:default"] and stripped ~= "window:create:external" and (prefix == "core" or
       stripped:match("^window:") or
       stripped:match("^shortcut:") or
       stripped:match("^tray:") or
       stripped == "menu:set" or
       (stripped:match("^state:") and stripped ~= "state:set" and stripped ~= "state:write") or
       stripped:match("^store:") or
       stripped:match("^path:") or
       stripped == "theme:get" or
       stripped == "event:emit" or
       stripped:match("^dialog:")) then
        return true
    end
    error(string.format("Capability non autorizzata: %s", name))
end

function Security:assert_program(program)
    self:assert_capability("shell:execute")
    local prog_str = tostring(program or "")
    if prog_str == "" or prog_str:find("%z") then
        error("Shell: invalid program name")
    end
    local lower_prog = prog_str:lower()
    if lower_prog:match("%.bat$") or lower_prog:match("%.cmd$") then
        error(string.format("Shell: direct execution of .bat / .cmd files is blocked for security (CWE-78): %s", prog_str))
    end
    if not self.allowed_programs or #self.allowed_programs == 0 then
        error(string.format("Shell: execution denied (allowed_programs is empty or missing): %s", prog_str))
    end
    local has_sep = prog_str:find("[/\\]") ~= nil
    local rule = self._program_map[prog_str]
    if not rule and not has_sep then
        local base_name = prog_str:match("[^/\\]+$") or prog_str
        rule = self._program_map[base_name]
    end
    if not rule then
        error(string.format("Shell: program not in allowlist: %s", prog_str))
    end
    return rule
end

function Security:assert_args(program, args)
    args = args or {}
    local prog_base = (tostring(program):match("[^/\\]+$") or tostring(program)):lower():gsub("%.exe$", "")

    for _, arg in ipairs(args) do
        local a = tostring(arg)
        if prog_base == "git" then
            if a == "-c" or a:match("^%-c")
               or a == "--exec-path" or a:match("^%-%-exec%-path")
               or a == "--upload-pack" or a:match("^%-%-upload%-pack")
               or a == "--receive-pack" or a:match("^%-%-receive%-pack")
               or a == "--config-env" or a:match("^%-%-config%-env")
               or a == "--pager" or a:match("^%-%-pager")
               or a:match("^ext::") then
                error(string.format("Shell: blocked dangerous argument for '%s': %s", tostring(program), a))
            end
        elseif (prog_base == "find" and (a == "-exec" or a == "-execdir" or a == "-ok")) or
               (prog_base == "xargs" and (a:match("^%-I") or a:match("^%-%-replace"))) or
               (prog_base == "ssh" and (a:lower():match("proxycommand"))) then
            error(string.format("Shell: blocked dangerous argument for '%s': %s", tostring(program), a))
        end
    end

    -- 2. Whitelist check
    if self._program_map["*"] == false then
        return -- wildcard any program and any args
    end

    local arg_policy = self._program_map[program]
    if arg_policy == nil and not tostring(program):find("[/\\]") then
        arg_policy = self._program_map[prog_base]
    end
    if arg_policy == false then
        -- Explicit opt-in for any arguments on this program
        return
    end

    if type(arg_policy) == "table" then
        if #arg_policy == 0 and #args > 0 then
            error(string.format("Shell: arguments not permitted for program '%s' (no arg allowlist defined)", tostring(program)))
        end
        local whiteset = {}
        for _, a in ipairs(arg_policy) do whiteset[tostring(a)] = true end
        for _, arg in ipairs(args) do
            if not whiteset[tostring(arg)] then
                error(string.format("Shell: argument not in allowlist for '%s': %s", tostring(program), tostring(arg)))
            end
        end
        return
    end

    -- If no arguments allowlist was configured, default deny non-empty arguments
    if #args > 0 then
        error(string.format("Shell: arguments not permitted for program '%s' (no arg allowlist defined)", tostring(program)))
    end
end

local function canonicalize_path(path)
    if type(path) ~= "string" or path == "" then
        return ""
    end
    if path:find("\0", 1, true) then
        error("Percorso non valido: contiene caratteri NUL")
    end
    local p_lower = path:lower()
    if p_lower:match("%%2e") or p_lower:match("%%2f") or p_lower:match("%%5c") or p_lower:match("%%00") then
        error("Percorso non valido: sequenze percent-encoding ambigue rilevate")
    end

    path = path:gsub("\\", "/")
    local drive, rest = path:match("^([a-zA-Z]:)[/\\]?(.*)$")
    local is_absolute = false
    if drive then
        is_absolute = true
        path = rest or ""
    elseif path:sub(1, 1) == "/" then
        is_absolute = true
        path = path:sub(2)
    end

    local parts = {}
    for part in path:gmatch("[^/]+") do
        if part == "." then
            -- skip
        elseif part == ".." then
            if #parts > 0 and parts[#parts] ~= ".." then
                table.remove(parts)
            else
                if not is_absolute then
                    table.insert(parts, "..")
                end
            end
        else
            table.insert(parts, part)
        end
    end

    local res = table.concat(parts, "/")
    if drive then
        res = drive .. "/" .. res
    elseif is_absolute then
        res = "/" .. res
    end
    return res
end

local function real_filesystem_path(path)
    if type(path) ~= "string" or path == "" then return "" end
    local canon = canonicalize_path(path)
    local has_sym, fn = pcall(function() return clib and clib.platform_resolve_path end)
    if has_sym and fn then
        local buf = ffi.new("char[4096]")
        local ok_call, res = pcall(fn, canon, buf, 4096)
        if ok_call and res == 1 then
            local resolved = ffi.string(buf)
            if resolved ~= "" then
                return resolved:gsub("\\", "/")
            end
        end
        error("Impossibile risolvere in modo canonico il percorso: " .. tostring(path))
    end
    return canon
end

local function glob_to_lua_pattern(glob_str)
    local s = glob_str:gsub("([%(%)%.%%%+%-%?%[%^%$])", "%%%1")
    s = s:gsub("%*%*", "\1")
    s = s:gsub("%*", "[^/]*")
    s = s:gsub("\1", ".*")
    return "^" .. s .. "$"
end

function Security:assert_scope(target_path, base_paths)
    if not self.scopes or #self.scopes == 0 then
        error(string.format("Accesso negato dallo Scope: nessuna regola di scope configurata (deny-by-default) per '%s'", tostring(target_path)))
    end
    if type(target_path) ~= "string" or target_path == "" then
        error("Percorso file non specificato o non valido")
    end

    local function substitute_base(str)
        return str:gsub("%$([A-Z_]+)", function(var)
            if var == "APPDATA" then return base_paths and base_paths.appData or ""
            elseif var == "HOME" then return base_paths and base_paths.home or ""
            elseif var == "DESKTOP" then return base_paths and base_paths.desktop or ""
            end
            return "$" .. var
        end)
    end

    local expanded_target = substitute_base(target_path)
    local normalized = real_filesystem_path(expanded_target)

    for _, pattern in ipairs(self.scopes) do
        if type(pattern) == "string" and pattern ~= "" then
            local resolved = substitute_base(pattern):gsub("\\", "/")
            local is_recursive = resolved:match("%*%*$") ~= nil
            local is_direct = (resolved:match("%*$") ~= nil) and not is_recursive
            local base_str = resolved:gsub("/%*%*?$", "")
            local canonical_base = real_filesystem_path(base_str)
            local escaped_base = canonical_base:gsub("([%(%)%.%%%+%-%?%[%^%$])", "%%%1")

            if is_recursive then
                if normalized == canonical_base or normalized:match("^" .. escaped_base .. "/.*$") then
                    return normalized
                end
            elseif is_direct then
                if normalized:match("^" .. escaped_base .. "/[^/]+$") then
                    return normalized
                end
            else
                local canonical_pat = real_filesystem_path(resolved)
                local pat = glob_to_lua_pattern(canonical_pat)
                if normalized == canonical_pat or normalized:match(pat) then
                    return normalized
                end
            end
        end
    end
    error(string.format("Accesso negato dallo Scope: %s", target_path))
end

local State = {}
State.__index = State

function State.new(json_codec)
    local self = setmetatable({}, State)
    self.json = json_codec
    self._versions = {}
    return self
end

function State:update(key, callback, max_retries)
    if type(callback) ~= "function" then
        error("State:update requires a function callback")
    end
    max_retries = max_retries or 50
    local skey = tostring(key)

    for _ = 1, max_retries do
        -- 1. Read current value and version snapshot under brief lock
        clib.platform_state_lock()
        local current = self:get(skey)
        local ver = self._versions[skey] or 0
        clib.platform_state_unlock()

        -- 2. Execute user callback strictly WITHOUT holding the mutex (prevents deadlocks during I/O)
        local ok, next_val = pcall(callback, current)
        if not ok then
            error(next_val)
        end

        -- 3. Atomic CAS commit: verify version hasn't changed, then write and increment version
        clib.platform_state_lock()
        local current_ver = self._versions[skey] or 0
        if current_ver == ver then
            if next_val ~= nil then
                self:set(skey, next_val)
            else
                self:delete(skey)
            end
            self._versions[skey] = current_ver + 1
            clib.platform_state_unlock()
            return next_val
        end
        clib.platform_state_unlock()
        -- Version changed: conflict detected, retry CAS loop
    end

    error(string.format("State:update failed: CAS conflict retry limit exceeded (%d attempts) for key '%s'", max_retries, skey))
end

function State:set(key, value)
    local vtype = type(value)
    local skey = tostring(key)
    if vtype == "number" then
        clib.platform_state_set_number(skey, value)
    elseif vtype == "string" then
        clib.platform_state_set_string(skey, value)
    elseif vtype == "table" then
        clib.platform_state_set_json(skey, self.json.encode(value))
    elseif vtype == "boolean" then
        clib.platform_state_set_string(skey, value and "true" or "false")
    elseif vtype == "nil" then
        clib.platform_state_delete(skey)
    end
end

function State:get(key)
    local k = tostring(key)
    if clib.platform_state_has(k) == 0 then
        return nil
    end

    local num_buf = ffi.new("double[1]")
    if clib.platform_state_get_number(k, num_buf) == 1 then
        return tonumber(num_buf[0])
    end

    local buf_size = 65536
    local s_buf = ffi.new("char[?]", buf_size)
    local rc_j = clib.platform_state_get_json(k, s_buf, buf_size)
    if rc_j > 1 then
        s_buf = ffi.new("char[?]", rc_j)
        rc_j = clib.platform_state_get_json(k, s_buf, rc_j)
    end
    if rc_j == 1 then
        local raw = ffi.string(s_buf)
        if raw == "true" then return true end
        if raw == "false" then return false end
        return self.json.decode(raw)
    end
    local rc_s = clib.platform_state_get_string(k, s_buf, buf_size)
    if rc_s > 1 then
        s_buf = ffi.new("char[?]", rc_s)
        rc_s = clib.platform_state_get_string(k, s_buf, rc_s)
    end
    if rc_s == 1 then
        local raw = ffi.string(s_buf)
        if raw == "true" then return true end
        if raw == "false" then return false end
        return raw
    end

    return nil
end

function State:has(key)
    return clib.platform_state_has(tostring(key)) == 1
end

function State:delete(key)
    clib.platform_state_delete(tostring(key))
end

function State:clear()
    clib.platform_state_clear()
end

local function safe_mkdir_p(target_path)
    if not target_path or target_path == "" or target_path == "." or target_path == "/" then
        return true
    end
    local normalized = target_path:gsub("\\", "/")
    local prefix = ""
    if normalized:match("^/") then
        prefix = "/"
        normalized = normalized:sub(2)
    elseif normalized:match("^[A-Za-z]:/") then
        prefix = normalized:sub(1, 3)
        normalized = normalized:sub(4)
    end

    local current = prefix
    for part in normalized:gmatch("[^/]+") do
        if part ~= "" and part ~= "." then
            current = (current == "" or current:match("/$")) and (current .. part) or (current .. "/" .. part)
            if ffi.os == "Windows" then
                pcall(function() clib._mkdir(current) end)
            else
                pcall(function() clib.mkdir(current, 493) end)
            end
        end
    end
    return true
end

local function with_state_lock(fn)
    local has_lock = clib and clib.platform_state_lock and clib.platform_state_unlock
    if has_lock then
        clib.platform_state_lock()
    end
    local ok, res, err = pcall(fn)
    if has_lock then
        clib.platform_state_unlock()
    end
    if not ok then
        error(res)
    end
    return res, err
end

local Store = {}
Store.__index = Store

function Store.new(store_path, json_codec)
    local self = setmetatable({}, Store)
    self.path = store_path
    self.json = json_codec
    self.data = {}
    self:load()
    return self
end

function Store:load()
    return with_state_lock(function()
        local f = io.open(self.path, "rb")
        if f then
            local raw = f:read("*a")
            f:close()
            self.data = self.json.decode(raw) or {}
        else
            self.data = {}
        end
        return self.data
    end)
end

function Store:save()
    return with_state_lock(function()
        local parent = self.path:match("^(.*)/[^/]+$")
        if parent and parent ~= "" then
            safe_mkdir_p(parent)
        end
        local tmp_path = self.path .. ".tmp"
        local f = io.open(tmp_path, "wb")
        if f then
            f:write(self.json.encode(self.data))
            f:flush()
            f:close()
            local ok = os.rename(tmp_path, self.path)
            if not ok then
                os.remove(self.path)
                ok = os.rename(tmp_path, self.path)
            end
            return ok == true or ok == 0
        end
        return false
    end)
end

function Store:get(key)
    return with_state_lock(function()
        self:load()
        return self.data[tostring(key)]
    end)
end

function Store:set(key, val)
    return with_state_lock(function()
        self:load()
        self.data[tostring(key)] = val
        return self:save()
    end)
end

function Store:has(key)
    return with_state_lock(function()
        self:load()
        return self.data[tostring(key)] ~= nil
    end)
end

function Store:delete(key)
    return with_state_lock(function()
        self:load()
        self.data[tostring(key)] = nil
        return self:save()
    end)
end

function Store:clear()
    return with_state_lock(function()
        self.data = {}
        return self:save()
    end)
end

function Store:entries()
    return with_state_lock(function()
        self:load()
        return self.data
    end)
end

-- ============================================================================
-- Native Menu Bar API (Tauri 2 / muda ergonomic parity in Lua)
-- ============================================================================

local function serialize_menu_item(item)
    if type(item) ~= "table" then return nil end
    if type(item.to_table) == "function" then
        return item:to_table()
    end
    local copy = {}
    for k, v in pairs(item) do
        if k == "items" and type(v) == "table" then
            copy.items = {}
            for _, sub_item in ipairs(v) do
                local s = serialize_menu_item(sub_item)
                if s then table.insert(copy.items, s) end
            end
        else
            copy[k] = v
        end
    end
    return copy
end

local MenuItem = {}
MenuItem.__index = MenuItem

function MenuItem.new(options)
    local self = setmetatable({}, MenuItem)
    options = options or {}
    self.type = "normal"
    self.id = options.id or tostring(math.random(100000, 999999))
    self.text = options.text or options.label or ""
    self.enabled = options.enabled ~= false
    self.accelerator = options.accelerator
    return self
end

function MenuItem:to_table()
    return {
        type = "normal",
        id = self.id,
        text = self.text,
        label = self.text,
        enabled = self.enabled,
        accelerator = self.accelerator
    }
end

local CheckMenuItem = {}
CheckMenuItem.__index = CheckMenuItem

function CheckMenuItem.new(options)
    local self = setmetatable({}, CheckMenuItem)
    options = options or {}
    self.type = "check"
    self.id = options.id or tostring(math.random(100000, 999999))
    self.text = options.text or options.label or ""
    self.checked = options.checked == true
    self.enabled = options.enabled ~= false
    self.accelerator = options.accelerator
    return self
end

function CheckMenuItem:to_table()
    return {
        type = "check",
        id = self.id,
        text = self.text,
        label = self.text,
        checked = self.checked,
        enabled = self.enabled,
        accelerator = self.accelerator
    }
end

local PredefinedMenuItem = {}
PredefinedMenuItem.__index = PredefinedMenuItem

function PredefinedMenuItem.new(role, options)
    local self = setmetatable({}, PredefinedMenuItem)
    options = options or {}
    self.type = (role == "separator") and "separator" or "predefined"
    self.role = role
    self.text = options.text or options.label
    self.enabled = options.enabled ~= false
    self.accelerator = options.accelerator
    return self
end

function PredefinedMenuItem:to_table()
    return {
        type = self.type,
        role = self.role,
        text = self.text,
        label = self.text,
        enabled = self.enabled,
        accelerator = self.accelerator
    }
end

function PredefinedMenuItem.separator()
    return PredefinedMenuItem.new("separator")
end

function PredefinedMenuItem.copy(text)
    return PredefinedMenuItem.new("copy", { text = text or "Copy", accelerator = "CmdOrCtrl+C" })
end

function PredefinedMenuItem.cut(text)
    return PredefinedMenuItem.new("cut", { text = text or "Cut", accelerator = "CmdOrCtrl+X" })
end

function PredefinedMenuItem.paste(text)
    return PredefinedMenuItem.new("paste", { text = text or "Paste", accelerator = "CmdOrCtrl+V" })
end

function PredefinedMenuItem.select_all(text)
    return PredefinedMenuItem.new("select_all", { text = text or "Select All", accelerator = "CmdOrCtrl+A" })
end

function PredefinedMenuItem.undo(text)
    return PredefinedMenuItem.new("undo", { text = text or "Undo", accelerator = "CmdOrCtrl+Z" })
end

function PredefinedMenuItem.redo(text)
    return PredefinedMenuItem.new("redo", { text = text or "Redo", accelerator = "CmdOrCtrl+Shift+Z" })
end

function PredefinedMenuItem.minimize(text)
    return PredefinedMenuItem.new("minimize", { text = text or "Minimize", accelerator = "CmdOrCtrl+M" })
end

function PredefinedMenuItem.zoom(text)
    return PredefinedMenuItem.new("zoom", { text = text or "Zoom" })
end

function PredefinedMenuItem.fullscreen(text)
    return PredefinedMenuItem.new("fullscreen", { text = text or "Toggle Full Screen" })
end

function PredefinedMenuItem.hide(text)
    return PredefinedMenuItem.new("hide", { text = text or "Hide", accelerator = "CmdOrCtrl+H" })
end

function PredefinedMenuItem.hide_others(text)
    return PredefinedMenuItem.new("hide_others", { text = text or "Hide Others" })
end

function PredefinedMenuItem.show_all(text)
    return PredefinedMenuItem.new("show_all", { text = text or "Show All" })
end

function PredefinedMenuItem.close_window(text)
    return PredefinedMenuItem.new("close_window", { text = text or "Close Window", accelerator = "CmdOrCtrl+W" })
end

function PredefinedMenuItem.quit(text)
    return PredefinedMenuItem.new("quit", { text = text or "Quit", accelerator = "CmdOrCtrl+Q" })
end

function PredefinedMenuItem.about(text)
    return PredefinedMenuItem.new("about", { text = text or "About" })
end

local Submenu = {}
Submenu.__index = Submenu

function Submenu.new(options_or_text, items, enabled)
    local self = setmetatable({}, Submenu)
    self.type = "submenu"
    self.items_list = {}
    if type(options_or_text) == "table" then
        self.text = options_or_text.text or options_or_text.label or ""
        self.enabled = options_or_text.enabled ~= false
        if options_or_text.items then
            for _, it in ipairs(options_or_text.items) do
                self:append(it)
            end
        end
    else
        self.text = tostring(options_or_text or "")
        self.enabled = enabled ~= false
        if type(items) == "table" then
            for _, it in ipairs(items) do
                self:append(it)
            end
        end
    end
    return self
end

function Submenu:append(item)
    table.insert(self.items_list, item)
    return self
end

function Submenu:to_table()
    local children = {}
    for _, it in ipairs(self.items_list) do
        local s = serialize_menu_item(it)
        if s then table.insert(children, s) end
    end
    return {
        type = "submenu",
        text = self.text,
        label = self.text,
        enabled = self.enabled,
        items = children
    }
end

local Menu = {}
Menu.__index = Menu

function Menu.new(options)
    local self = setmetatable({}, Menu)
    self.items_list = {}
    if type(options) == "table" and options.items then
        for _, it in ipairs(options.items) do
            self:append(it)
        end
    elseif type(options) == "table" and #options > 0 then
        for _, it in ipairs(options) do
            self:append(it)
        end
    end
    return self
end

function Menu:append(item)
    table.insert(self.items_list, item)
    return self
end

function Menu:items()
    return self.items_list
end

function Menu:to_table()
    local out = { items = {} }
    for _, it in ipairs(self.items_list) do
        local s = serialize_menu_item(it)
        if s then table.insert(out.items, s) end
    end
    return out
end

function Menu.default()
    return Menu.new({
        items = {
            Submenu.new({
                text = "File",
                items = {
                    PredefinedMenuItem.close_window(),
                }
            }),
            Submenu.new({
                text = "Edit",
                items = {
                    PredefinedMenuItem.undo(),
                    PredefinedMenuItem.redo(),
                    PredefinedMenuItem.separator(),
                    PredefinedMenuItem.cut(),
                    PredefinedMenuItem.copy(),
                    PredefinedMenuItem.paste(),
                    PredefinedMenuItem.select_all(),
                }
            }),
            Submenu.new({
                text = "View",
                items = {
                    PredefinedMenuItem.fullscreen(),
                }
            }),
            Submenu.new({
                text = "Window",
                items = {
                    PredefinedMenuItem.minimize(),
                    PredefinedMenuItem.zoom(),
                    PredefinedMenuItem.separator(),
                    PredefinedMenuItem.close_window(),
                }
            }),
        }
    })
end

local TAURI_CLIENT_SCRIPT = [[
(function() {
  var channelSeq = 0;
  var channelMap = {};

  function Channel(onMessage) {
    this.id = ++channelSeq;
    this.onmessage = onMessage || null;
    channelMap[this.id] = this;
  }
  Channel.prototype.send = function(data) {
    if (typeof this.onmessage === "function") {
      this.onmessage(data);
    }
  };
  Channel.prototype.close = function() {
    delete channelMap[this.id];
  };
  Channel.prototype.toJSON = function() {
    return { __tauri_channel__: this.id };
  };

  window.__TAURI_CHANNELS__ = channelMap;
  window.__TAURI_DISPATCH_CHANNEL__ = function(id, data) {
    var ch = channelMap[id];
    if (ch && typeof ch.onmessage === "function") {
      ch.onmessage(data);
    }
  };

  function makeInvoke(cmd, payload) {
    return new Promise(function(resolve, reject) {
      var raw = JSON.stringify([cmd, payload || {}, window.__TAURI_TOKEN__ || ""]);
      window.__TAURI_RAW_INVOKE__(raw).then(function(res) {
        try {
          var parsed = JSON.parse(res);
          if (parsed && parsed.error) {
            reject(parsed.error);
          } else {
            resolve(parsed !== undefined && parsed.data !== undefined ? parsed.data : parsed);
          }
        } catch(e) {
          resolve(res);
        }
      }).catch(function(err) {
        try {
          var parsed = JSON.parse(err);
          reject(parsed && parsed.error ? parsed.error : err);
        } catch(e) {
          reject(err);
        }
      });
    });
  }

  document.addEventListener("mousedown", function(e) {
    var target = e.target;
    while (target && target !== document.body) {
      if (target.hasAttribute("data-tauri-drag-region")) {
        makeInvoke("tauri:window:start_dragging", {});
        break;
      }
      target = target.parentElement;
    }
  });

  window.__TAURI__ = {
    core: { invoke: makeInvoke },
    invoke: makeInvoke,
    Channel: Channel,
    channel: { Channel: Channel },
    state: {
      get: function(key) { return makeInvoke("tauri:state:get", { key: key }); },
      set: function(key, val) { return makeInvoke("tauri:state:set", { key: key, value: val }); }
    },
    event: {
      _handlers: {},
      listen: function(event, cb) {
        if (!this._handlers[event]) this._handlers[event] = [];
        this._handlers[event].push(cb);
        return Promise.resolve(function() {
          if (window.__TAURI__.event._handlers[event]) {
            window.__TAURI__.event._handlers[event] = window.__TAURI__.event._handlers[event].filter(function(fn) { return fn !== cb; });
          }
        });
      },
      emit: function(event, payload) {
        return makeInvoke("tauri:event:emit", { event: event, payload: payload });
      },
      _dispatch: function(event, payload) {
        if (this._handlers[event]) {
          var list = this._handlers[event].slice();
          for (var i = 0; i < list.length; i++) list[i]({ event: event, payload: payload });
        }
      }
    },
    webviewWindow: {
      WebviewWindow: function(label, options) {
        options = options || {};
        options.label = label;
        makeInvoke("tauri:window:create", options);
        return {
          label: label,
          setTitle: function(t) { return makeInvoke("tauri:window:set_title", { label: label, title: t }); },
          setSize: function(w, h) { return makeInvoke("tauri:window:set_size", { label: label, width: w, height: h }); },
          close: function() { return makeInvoke("tauri:window:close", { label: label }); },
          destroy: function() { return makeInvoke("tauri:window:destroy", { label: label }); },
          onCloseRequested: function(handler) {
            return window.__TAURI__.event.listen("tauri://close-requested", function(e) {
              if (!e.payload || e.payload.label === label) handler(e);
            });
          },
          hide: function() { return makeInvoke("tauri:window:hide", { label: label }); },
          show: function() { return makeInvoke("tauri:window:show", { label: label }); },
          minimize: function() { return makeInvoke("tauri:window:minimize", { label: label }); },
          maximize: function() { return makeInvoke("tauri:window:maximize", { label: label }); }
        };
      },
      getCurrentWebviewWindow: function() {
        return {
          setTitle: function(t) { return makeInvoke("tauri:window:set_title", { title: t }); },
          setSize: function(w, h) { return makeInvoke("tauri:window:set_size", { width: w, height: h }); },
          close: function() { return makeInvoke("tauri:window:close", {}); },
          destroy: function() { return makeInvoke("tauri:window:destroy", {}); },
          onCloseRequested: function(handler) {
            return window.__TAURI__.event.listen("tauri://close-requested", handler);
          },
          hide: function() { return makeInvoke("tauri:window:hide", {}); },
          show: function() { return makeInvoke("tauri:window:show", {}); },
          minimize: function() { return makeInvoke("tauri:window:minimize", {}); },
          maximize: function() { return makeInvoke("tauri:window:maximize", {}); },
          setFocus: function() { return makeInvoke("tauri:window:set_focus", {}); },
          theme: function() { return makeInvoke("tauri:theme:get", {}); },
          onDragDropEvent: function(handler) {
            return window.__TAURI__.event.listen("tauri://drag-drop", function(e) {
              handler(e.payload || e);
            });
          },
          startDragging: function() { return makeInvoke("tauri:window:start_dragging", {}); }
        };
      }
    },
    menu: {
      onMenuClicked: function(handler) {
        return window.__TAURI__.event.listen("tauri://menu-event", function(e) {
          handler(e.payload || e);
        });
      },
      setMenu: function(menu) {
        return makeInvoke("tauri:menu:set", { menu: menu });
      }
    },
    tray: {
      TrayIcon: {
        new: function(options) {
          return makeInvoke("tauri:tray:create", options || {});
        }
      }
    },
    globalShortcut: {
      register: function(shortcut, handler) {
        window.__TAURI__.event.listen("tauri://shortcut-" + shortcut, handler);
        return makeInvoke("tauri:shortcut:register", { shortcut: shortcut });
      },
      unregister: function(shortcut) {
        return makeInvoke("tauri:shortcut:unregister", { shortcut: shortcut });
      }
    },
    deepLink: {
      onOpenUrl: function(handler) {
        return window.__TAURI__.event.listen("tauri://deep-link", handler);
      }
    },
    updater: {
      check: function() {
        return makeInvoke("tauri:updater:check", {});
      },
      downloadAndInstall: function() {
        return makeInvoke("tauri:updater:install", {});
      }
    },
    fs: {
      readTextFile: function(p) { return makeInvoke("tauri:fs:read_text_file", { path: p }); },
      writeTextFile: function(p, c) { return makeInvoke("tauri:fs:write_text_file", { path: p, contents: c }); },
      readDir: function(p) { return makeInvoke("tauri:fs:read_dir", { path: p }); },
      remove: function(p) { return makeInvoke("tauri:fs:remove", { path: p }); }
    },
    dialog: {
      open: function(o) { return makeInvoke("tauri:dialog:open", o || {}); },
      save: function(o) { return makeInvoke("tauri:dialog:save", o || {}); },
      message: function(m, o) { o = o || {}; return makeInvoke("tauri:dialog:message", { message: m, title: o.title || "Message", kind: o.kind || o.type }); },
      ask: function(m, o) { o = o || {}; return makeInvoke("tauri:dialog:ask", { message: m, title: o.title || "Confirm", kind: o.kind || o.type }); }
    },
    clipboard: {
      readText: function() { return makeInvoke("tauri:clipboard:read_text", {}); },
      writeText: function(t) { return makeInvoke("tauri:clipboard:write_text", { text: t }); },
      readHtml: function() { return makeInvoke("tauri:clipboard:read_html", {}); },
      writeHtml: function(h) { return makeInvoke("tauri:clipboard:write_html", { html: h }); },
      readImage: function() { return makeInvoke("tauri:clipboard:read_image", {}); },
      writeImage: function(img) { return makeInvoke("tauri:clipboard:write_image", { image: img }); },
      clear: function() { return makeInvoke("tauri:clipboard:clear", {}); }
    },
    window: {
      theme: function() { return makeInvoke("tauri:theme:get", {}); }
    },
    notification: {
      isPermissionGranted: function() { return Promise.resolve(true); },
      requestPermission: function() { return Promise.resolve("granted"); },
      sendNotification: function(o) { if (typeof o === "string") o = { title: o }; return makeInvoke("tauri:notification:send", o); }
    },
    singleInstance: {
      onSecondInstance: function(cb) {
        return window.__TAURI__.event.listen("tauri://single-instance", function(ev) {
          if (typeof cb === "function") cb(ev.payload);
        });
      }
    },
    store: {
      get: function(k) { return makeInvoke("tauri:store:get", { key: k }); },
      set: function(k, v) { return makeInvoke("tauri:store:set", { key: k, value: v }); },
      has: function(k) { return makeInvoke("tauri:store:has", { key: k }); },
      delete: function(k) { return makeInvoke("tauri:store:delete", { key: k }); },
      clear: function() { return makeInvoke("tauri:store:clear", {}); }
    },
    shell: {
      execute: function(cmd, args, onOutput) {
        if (typeof args === "function") {
          onOutput = args;
          args = [];
        }
        var ch = onOutput ? new Channel(onOutput) : undefined;
        if (Array.isArray(args)) {
          return makeInvoke("tauri:shell:execute", { program: cmd, args: args, onProgress: ch });
        } else {
          return makeInvoke("tauri:shell:execute", { command: cmd, onProgress: ch });
        }
      }
    },
    path: {
      appDataDir: function() { return makeInvoke("tauri:path:app_data_dir", {}); },
      homeDir: function() { return makeInvoke("tauri:path:home_dir", {}); },
      desktopDir: function() { return makeInvoke("tauri:path:desktop_dir", {}); }
    }
  };
})();
]]


-- ============================================================================
-- Native Sidecar Process Manager
-- Spawns, manages, and terminates background native binaries bundled with the app.
-- Automatically resolves target architecture, platform bundles, and lifecycle.
-- ============================================================================
local Sidecar = {}
Sidecar.__index = Sidecar

local function file_readable(path)
    if not path or path == '' then return false end
    local f = io.open(path, 'r')
    if f then
        f:close()
        return true
    end
    return false
end
local file_exists = file_readable

local function sh_quote(s)
    s = tostring(s or '')
    if ffi.os == "Windows" then
        s = s:gsub('"', '""')
        return '"' .. s .. '"'
    else
        return "'" .. s:gsub("'", "'\\''") .. "'"
    end
end

local FORBIDDEN_ENV_KEYS = {
    LD_PRELOAD = true, LD_LIBRARY_PATH = true, LD_AUDIT = true,
    DYLD_INSERT_LIBRARIES = true, DYLD_LIBRARY_PATH = true, DYLD_FRAMEWORK_PATH = true,
    PATH = true, IFS = true, BASH_ENV = true, ENV = true,
    NODE_OPTIONS = true, PYTHONSTARTUP = true, PYTHONPATH = true,
    LUA_PATH = true, LUA_CPATH = true, LUA_INIT = true
}

local function is_safe_env_key(k)
    if type(k) ~= "string" or not k:match("^[a-zA-Z_][a-zA-Z0-9_]*$") then
        return false
    end
    if FORBIDDEN_ENV_KEYS[k:upper()] then
        return false
    end
    return true
end

local function parse_env_file(path)
    local vars = {}
    local f = io.open(path, 'r')
    if not f then return vars end
    for line in f:lines() do
        line = line:gsub('^%s+', ''):gsub('%s+$', '')
        if line ~= '' and not line:match('^#') then
            local k, v = line:match('^([%w_]+)%s*=%s*(.*)$')
            if k and v then
                if (v:sub(1,1) == '"' and v:sub(-1,-1) == '"') or
                   (v:sub(1,1) == "'" and v:sub(-1,-1) == "'") then
                    v = v:sub(2, -2)
                end
                vars[k] = v
            end
        end
    end
    f:close()
    return vars
end

function Sidecar.resolve_path(name, app_name)
    if not name or name == '' then return nil end
    app_name = app_name or 'tauri-lua-app'

    local is_dev = (os.getenv("TAURILUA_DEV") == "1") or (rawget(_G, "__TAURI_DEV__") == true)
    local os_name = ffi.os
    local arch_name = ffi.arch
    local norm_arch = (arch_name == 'arm64' or arch_name == 'aarch64') and 'arm64' or 'amd64'

    -- Generate target binary candidates
    local candidates = {}
    if os_name == 'Windows' then
        table.insert(candidates, name .. '.exe')
        table.insert(candidates, name .. '-windows-' .. norm_arch .. '.exe')
        table.insert(candidates, name)
    elseif os_name == 'Linux' then
        table.insert(candidates, name .. '-linux-' .. norm_arch)
        if norm_arch == 'arm64' or norm_arch == 'aarch64' then
            table.insert(candidates, name .. '-linux-arm64')
            table.insert(candidates, name .. '-linux-aarch64')
        else
            table.insert(candidates, name .. '-linux-amd64')
            table.insert(candidates, name .. '-linux-x86_64')
            table.insert(candidates, name .. '-linux-x64')
        end
        table.insert(candidates, name)
    else -- OSX / macOS
        table.insert(candidates, name .. '-darwin-' .. norm_arch)
        table.insert(candidates, name .. '-macos-' .. norm_arch)
        table.insert(candidates, name)
    end

    local exec_dir = get_executable_dir()
    local search_dirs = {}

    -- 1. macOS Bundle Resources (.app/Contents/Resources/binaries)
    if exec_dir and exec_dir:match('/Contents/MacOS$') then
        local res_dir = exec_dir:gsub('/MacOS$', '/Resources')
        table.insert(search_dirs, res_dir .. '/binaries')
        table.insert(search_dirs, res_dir)
    end

    -- 2. Linux System packages (/usr/share/<app-name>/binaries)
    if os_name == 'Linux' then
        local clean_name = app_name:gsub('[^%w_%- ]', '')
        local slug_name = clean_name:lower():gsub('%s+', '-')
        table.insert(search_dirs, '/usr/share/' .. clean_name .. '/binaries')
        table.insert(search_dirs, '/usr/share/' .. slug_name .. '/binaries')
        table.insert(search_dirs, '/usr/local/share/' .. clean_name .. '/binaries')
        table.insert(search_dirs, '/usr/local/share/' .. slug_name .. '/binaries')
        table.insert(search_dirs, '/opt/' .. clean_name .. '/binaries')
        table.insert(search_dirs, '/opt/' .. slug_name .. '/binaries')
        local appdir = os.getenv('APPDIR')
        if appdir and appdir ~= '' then
            table.insert(search_dirs, appdir .. '/usr/share/' .. clean_name .. '/binaries')
            table.insert(search_dirs, appdir .. '/usr/share/' .. slug_name .. '/binaries')
        end
        if exec_dir then
            table.insert(search_dirs, exec_dir .. '/../share/' .. clean_name .. '/binaries')
            table.insert(search_dirs, exec_dir .. '/../share/' .. slug_name .. '/binaries')
        end
    end

    -- 3. Standalone binary-adjacent paths
    if exec_dir then
        table.insert(search_dirs, exec_dir .. '/binaries')
        table.insert(search_dirs, exec_dir)
    end

    -- 4. Development mode relative paths
    if is_dev then
        table.insert(search_dirs, 'src-tauri/binaries')
        table.insert(search_dirs, 'binaries')
        table.insert(search_dirs, './src-tauri/binaries')
        table.insert(search_dirs, './binaries')
    end

    for _, dir in ipairs(search_dirs) do
        for _, bin_file in ipairs(candidates) do
            local full_path = dir .. '/' .. bin_file
            if file_readable(full_path) then
                if os_name ~= 'Windows' and is_dev then
                    pcall(function() os.execute('chmod +x ' .. sh_quote(full_path) .. ' 2>/dev/null') end)
                end
                return full_path
            end
        end
    end

    if is_dev and file_readable(name) then
        return name
    end

    return nil
end

function Sidecar.find_env_file(explicit_path, binary_path, app_name)
    local explicit = explicit_path
    local bin = binary_path
    local app = app_name
    if explicit and type(explicit) == "string" and not explicit:find("%.env$") and bin and type(bin) == "string" and not bin:find("[/\\]") then
        explicit, bin, app = app, explicit, bin
    end
    if explicit and explicit ~= "" and file_exists(explicit) then
        return explicit
    end
    local is_dev = (os.getenv("TAURILUA_DEV") == "1") or (_G.__TAURI_DEV__ == true)
    local candidates = {}
    if is_dev then
        table.insert(candidates, "src-tauri/binaries/.env")
        table.insert(candidates, ".env")
    end
    local home = os.getenv("HOME") or os.getenv("USERPROFILE")
    if home and app and app ~= "" then
        if ffi.os == "OSX" then
            table.insert(candidates, home .. "/Library/Application Support/" .. app .. "/.env")
        elseif ffi.os == "Windows" then
            local appdata = os.getenv("APPDATA") or (home .. "\\AppData\\Roaming")
            table.insert(candidates, appdata .. "\\" .. app .. "\\.env")
        else
            table.insert(candidates, home .. "/.config/" .. app .. "/.env")
        end
    end
    if bin and bin ~= "" then
        local bin_dir = bin:match("^(.*)[/\\][^/\\]+$")
        if bin_dir then
            table.insert(candidates, bin_dir .. "/.env")
        end
    end
    local exe_dir = get_executable_dir()
    if exe_dir then
        table.insert(candidates, exe_dir .. "/../Resources/binaries/.env")
        table.insert(candidates, exe_dir .. "/binaries/.env")
        if app and app ~= "" then
            table.insert(candidates, "/usr/share/" .. app .. "/binaries/.env")
        end
    end
    for _, c in ipairs(candidates) do
        if file_exists(c) then
            return c
        end
    end
    return nil
end

function Sidecar.spawn(self_or_name, name_or_opts, maybe_opts)
    local app_instance = nil
    local name = nil
    local opts = nil

    if type(self_or_name) == 'table' and (self_or_name.config or self_or_name == Tauri) then
        app_instance = self_or_name
        name = name_or_opts
        opts = maybe_opts or {}
    else
        name = self_or_name
        opts = name_or_opts or {}
        app_instance = maybe_opts or g_tauri_instance
    end

    local app_name = (app_instance and app_instance.config and (app_instance.config.productName or (app_instance.config.package and app_instance.config.package.name))) or 'tauri-lua-app'
    local bin_path = Sidecar.resolve_path(name, app_name)
    if not bin_path then
        error(string.format("Sidecar binary '%s' not found for platform %s (%s)", tostring(name), ffi.os, ffi.arch))
    end

    local env_file = Sidecar.find_env_file(opts.env_file, bin_path, app_name)
    local env_vars = env_file and parse_env_file(env_file) or {}

    if opts.env and type(opts.env) == 'table' then
        for k, v in pairs(opts.env) do
            env_vars[k] = tostring(v)
        end
    end

    local port = opts.port or tonumber(env_vars['PORT']) or 8060
    env_vars['PORT'] = tostring(port)
    if not env_vars['FRONTEND_URL'] or env_vars['FRONTEND_URL'] == '' or env_vars['FRONTEND_URL'] == 'http://localhost:5173' then
        env_vars['FRONTEND_URL'] = '*'
    end

    local host = opts.host or '127.0.0.1'
    local bin_dir = opts.cwd or bin_path:match('^(.*)/[^/]+$') or '.'
    local log_file = opts.log_file
    if not log_file or log_file == '' then
        if ffi.os == 'Windows' then
            log_file = 'NUL'
        else
            local tmp_f = string.format('/tmp/%s_%s.log', app_name:lower():gsub('[^%w_]', '_'), tostring(name):lower():gsub('[^%w_]', '_'))
            local f_try = io.open(tmp_f, 'w')
            if f_try then
                f_try:close()
                log_file = tmp_f
            else
                log_file = '/dev/null'
            end
        end
    end

    local sc = setmetatable({
        name = name,
        app_name = app_name,
        binary_path = bin_path,
        env_file = env_file,
        env = env_vars,
        host = host,
        port = port,
        base_url = string.format('http://%s:%d', host, port),
        log_file = log_file,
        pid = nil,
        auto_kill = opts.auto_kill ~= false
    }, Sidecar)

    local env_parts = {}
    for k, v in pairs(env_vars) do
        if is_safe_env_key(k) then
            table.insert(env_parts, k .. '=' .. sh_quote(v))
        end
    end
    local env_str = table.concat(env_parts, ' ')
    if env_str ~= '' then env_str = env_str .. ' ' end

    local args_parts = {}
    if opts.args and type(opts.args) == 'table' then
        for _, a in ipairs(opts.args) do
            table.insert(args_parts, sh_quote(a))
        end
    elseif opts.port then
        table.insert(args_parts, '--port')
        table.insert(args_parts, sh_quote(port))
    end
    local args_str = #args_parts > 0 and (' ' .. table.concat(args_parts, ' ')) or ''

    if ffi.os == 'Windows' then
        local win_args = ''
        if #args_parts > 0 then win_args = ' ' .. table.concat(args_parts, ' ') end
        local cmd = string.format('start /B "" %s%s > %s 2>&1', sh_quote((bin_path:gsub("/", [[\]]))), win_args, sh_quote((log_file:gsub("/", [[\]]))))
        os.execute(cmd)
    else
        local cmd = string.format('cd %s && %s%s%s > %s 2>&1 & echo $!',
            sh_quote(bin_dir),
            env_str,
            sh_quote(bin_path),
            args_str,
            sh_quote(log_file)
        )
        local handle = io.popen(cmd)
        if handle then
            local pid_str = handle:read('*l')
            handle:close()
            sc.pid = tonumber(pid_str)
            if sc.pid and sc.pid > 0 and clib and clib.platform_register_child_pid then
                pcall(clib.platform_register_child_pid, sc.pid)
            end
        end
    end

    if app_instance then
        app_instance.sidecars = app_instance.sidecars or {}
        table.insert(app_instance.sidecars, sc)

        if opts.ipc_command then
            app_instance:register(opts.ipc_command, function()
                return sc:to_table()
            end)
        end
    end

    return sc
end

function Sidecar:is_running()
    if not self.pid or self.pid <= 0 then return false end
    if ffi.os == 'Windows' then return true end
    local code = os.execute(string.format('kill -0 %d 2>/dev/null', self.pid))
    return code == 0
end

function Sidecar:kill(sig)
    if self.pid and self.pid > 0 then
        if clib and clib.platform_unregister_child_pid then
            pcall(clib.platform_unregister_child_pid, self.pid)
        end
        local s = sig or 15
        pcall(os.execute, string.format('kill -%d %d 2>/dev/null', s, self.pid))
        if not sig then
            pcall(os.execute, string.format('(sleep 0.1 && kill -9 %d 2>/dev/null) &', self.pid))
        end
        self.pid = nil
    end
    return true
end

function Sidecar:stop()
    return self:kill()
end

function Sidecar:to_table()
    return {
        name = self.name,
        pid = self.pid,
        host = self.host,
        port = self.port,
        base_url = self.base_url,
        running = self:is_running(),
        binary_path = self.binary_path,
        log_file = self.log_file
    }
end

local Tauri = {}
Tauri.__index = Tauri
Tauri._filter_absolute_only = filter_absolute_only

local g_tauri_instance = nil

function Tauri.new(config)
    local self = setmetatable({}, Tauri)
    if (not config or next(config) == nil) and __EMBEDDED_CONFIG_JSON__ and __EMBEDDED_CONFIG_JSON__ ~= "" then
        local ok, decoded = pcall(json.decode, __EMBEDDED_CONFIG_JSON__)
        if ok and type(decoded) == "table" then
            config = decoded
        end
    end
    config = config or {}
    self.config = config
    setup_module_search_paths(self.config)
    local app_name = (config.package and config.package.name) or config.productName or "tauri-lua-app"
    if not __IS_ISOLATE_WORKER__ then
        pcall(function() clib.platform_set_app_name(app_name) end)
    end
    self.security = Security.new(config)
    self.commands = {}
    self.listeners = {}
    self.windows = {}
    self.plugins = {}
    self.plugin_list = {}
    self.menu_listeners = {}
    self.window_listeners = {}
    self.single_instance_listeners = {}
    self.theme_listeners = {}
    self.file_drop_listeners = {}
    local ok_theme, theme_val = pcall(function() return ffi.string(clib.platform_get_theme()) end)
    self._current_theme = ok_theme and theme_val or "light"
    self.json = json
    self.sidecars = {}
    self.state = State.new(json)

    self.clipboard = {
        read_text = function()
            local buf = ffi.new("char[16384]")
            local ok = clib.platform_clipboard_read(buf, 16384)
            return ok == 1 and ffi.string(buf) or ""
        end,
        write_text = function(self_or_text, maybe_text)
            local text = (maybe_text ~= nil) and maybe_text or self_or_text
            return clib.platform_clipboard_write(tostring(text or "")) == 1
        end,
        read_html = function()
            local buf = ffi.new("char[65536]")
            local ok = clib.platform_clipboard_read_html(buf, 65536)
            return ok == 1 and ffi.string(buf) or ""
        end,
        write_html = function(self_or_html, maybe_html)
            local html = (maybe_html ~= nil) and maybe_html or self_or_html
            return clib.platform_clipboard_write_html(tostring(html or "")) == 1
        end,
        read_image = function()
            local buf = ffi.new("char[2097152]")
            local ok = clib.platform_clipboard_read_image(buf, 2097152)
            return ok == 1 and ffi.string(buf) or ""
        end,
        write_image = function(self_or_b64, maybe_b64)
            local b64 = (maybe_b64 ~= nil) and maybe_b64 or self_or_b64
            return clib.platform_clipboard_write_image(tostring(b64 or "")) == 1
        end,
        clear = function()
            clib.platform_clipboard_clear()
        end
    }

    local sec_conf = (config.app and config.app.security) or (config.tauri and config.tauri.security) or config.security or {}
    if sec_conf.csp and sec_conf.csp ~= "" then
        clib.platform_set_csp(sec_conf.csp)
    end

    self:_resolve_paths()
    local app_id = config.identifier or "com.tauri.lua"
    local store_file = self.base_paths.appData .. "/" .. app_id .. "/store.json"
    self.store = Store.new(store_file, json)

    self:_setup_asset_scheme()
    self:_register_builtins()

    g_tauri_instance = self
    return self
end

function Tauri:set_csp(csp)
    if type(csp) == "string" and csp ~= "" then
        clib.platform_set_csp(csp)
    end
end

function Tauri:spawn_sidecar(name, opts)
    return Sidecar.spawn(self, name, opts)
end

function Tauri:sidecar(name, opts)
    return Sidecar.spawn(self, name, opts)
end

function Tauri:on_window_event(cb)
    if type(cb) == "function" then
        table.insert(self.window_listeners, cb)
    end
    return self
end

function Tauri:force_close_window(label)
    label = label or "main"
    clib.platform_window_force_close(label)
    self.windows[label] = nil
    return true
end

function Tauri:destroy_window(label)
    return self:force_close_window(label)
end

function Tauri:set_window_focus(label)
    clib.platform_window_set_focus(label or "main")
    return self
end

function Tauri:on_single_instance(cb)
    if type(cb) == "function" then
        table.insert(self.single_instance_listeners, cb)
    end
    return self
end

function Tauri:enable_single_instance(cb)
    if cb then
        self:on_single_instance(cb)
    end
    local payload = rawget(_G, "__TAURI_STARTUP_PAYLOAD__")
    if not payload or payload == "" then
        payload = json.encode({ args = arg or {}, cwd = self.base_paths.home or "" })
    end
    local app_id = self.config.identifier or (self.config.package and self.config.package.name) or "com.tauri.lua"
    local res = clib.platform_single_instance_init(app_id, payload)
    if res == 0 then
        os.exit(0)
    end
    return res == 1
end

function Tauri:notify(title, body)
    if type(title) == "table" then
        body = title.body or title.message or ""
        title = title.title or "App"
    end
    clib.platform_notification_send(tostring(title or "App"), tostring(body or ""))
end

function Tauri:get_theme()
    return ffi.string(clib.platform_get_theme())
end

function Tauri:on_theme_changed(cb)
    if type(cb) == "function" then
        table.insert(self.theme_listeners, cb)
    end
    return self
end

function Tauri:on_file_drop(cb)
    if type(cb) == "function" then
        table.insert(self.file_drop_listeners, cb)
    end
    return self
end

function Tauri:set_app_name(name)
    if type(name) == "string" and name ~= "" then
        pcall(function() clib.platform_set_app_name(name) end)
    end
    return self
end

function Tauri:set_menu(menu_def)
    local payload = nil
    if type(menu_def) == "table" and type(menu_def.to_table) == "function" then
        payload = menu_def:to_table()
    elseif type(menu_def) == "table" then
        payload = serialize_menu_item(menu_def)
    else
        error("set_menu: expected Menu instance or table definition")
    end
    local json_str = json.encode(payload)
    clib.platform_set_menu(json_str)
    return true
end

function Tauri:on_menu(cb)
    if type(cb) == "function" then
        table.insert(self.menu_listeners, cb)
    end
    return self
end

function Tauri.sleep(ms)
    ms = tonumber(ms) or 0
    if ffi.os == "Windows" then
        clib.Sleep(ms)
    else
        clib.usleep(ms * 1000)
    end
end

function Tauri:_resolve_paths()
    local home = os.getenv("HOME") or os.getenv("USERPROFILE")
    if not home or home == "" then
        error("Impossibile risolvere la directory utente: variabili HOME e USERPROFILE mancanti (fail-closed)")
    end
    local app_data = os.getenv("APPDATA") or (home .. "/.config")
    local desktop = home .. "/Desktop"
    self.base_paths = {
        home = home:gsub("\\", "/"),
        appData = app_data:gsub("\\", "/"),
        desktop = desktop:gsub("\\", "/")
    }
end

function Tauri:_setup_asset_scheme()
    local build_conf = self.config.build or {}
    local tauri_conf = self.config.tauri or {}
    local bundle_conf = tauri_conf.bundle or self.config.bundle or {}

    local dist_dir = build_conf.frontendDist or build_conf.distDir or bundle_conf.distDir or "dist"
    -- Strip leading "../" if running from root
    if dist_dir:sub(1, 3) == "../" then
        dist_dir = dist_dir:sub(4)
    end

    local exec_dir, exe_name = get_executable_info()
    local app_name = (self.config.package and self.config.package.name) or self.config.productName or "app"
    local slug_name = slugify_package_name(app_name)
    local candidates = {
        dist_dir,
        "/usr/share/" .. slug_name .. "/" .. dist_dir,
        "/usr/share/" .. slug_name,
        "/usr/local/share/" .. slug_name .. "/" .. dist_dir,
        "/usr/local/share/" .. slug_name
    }
    if exe_name then
        table.insert(candidates, "/usr/share/" .. exe_name .. "/" .. dist_dir)
        table.insert(candidates, "/usr/share/" .. exe_name)
        table.insert(candidates, "/usr/local/share/" .. exe_name .. "/" .. dist_dir)
        table.insert(candidates, "/usr/local/share/" .. exe_name)
    end
    if exec_dir then
        table.insert(candidates, exec_dir .. "/../share/" .. slug_name .. "/" .. dist_dir)
        table.insert(candidates, exec_dir .. "/../share/" .. slug_name)
        if exe_name then
            table.insert(candidates, exec_dir .. "/../share/" .. exe_name .. "/" .. dist_dir)
            table.insert(candidates, exec_dir .. "/../share/" .. exe_name)
        end
        table.insert(candidates, exec_dir .. "/" .. dist_dir)
        table.insert(candidates, exec_dir)
    end
    local chosen_dist = dist_dir
    for _, c in ipairs(candidates) do
        local f = io.open(c .. "/index.html", "r")
        if f then
            f:close()
            chosen_dist = c
            break
        end
    end
    pcall(function() clib.platform_set_assets_path(chosen_dist) end)

    local dev_url = build_conf.devUrl or build_conf.devPath
    if dev_url and dev_url ~= "" then
        pcall(function() clib.bridge_set_dev_url(dev_url) end)
    end
    if __TAURI_DEV__ and dev_url and dev_url ~= "" then
        self.default_origin = dev_url
        if not self.default_origin:match("/$") and not self.default_origin:match("%.html$") then
            self.default_origin = self.default_origin .. "/"
        end
    elseif ffi.os == "Windows" then
        self.default_origin = "https://tauri.localhost/"
    else
        self.default_origin = "tauri://localhost/"
    end
end

local function is_trusted_origin(url_str)
    if not url_str or url_str == "" then return false end
    if clib and clib.platform_is_origin_allowed then
        return clib.platform_is_origin_allowed(url_str) == 1
    end
    if url_str:find("@", 1, true) then return false end
    local lower = url_str:lower()
    if lower == "tauri://localhost" or lower:match("^tauri://localhost[/%?#]") then
        return true
    end
    if lower == "https://tauri.localhost" or lower:match("^https://tauri%.localhost[/%?#]") then
        return true
    end
    return false
end

function Tauri:_register_builtins()
    self:register("tauri:state:get", function(payload)
        self.security:assert_capability("tauri:state:get")
        return self.state:get(payload and payload.key)
    end)

    self:register("tauri:state:set", function(payload)
        self.security:assert_capability("tauri:state:set")
        self.state:set(payload and payload.key, payload and payload.value)
        return true
    end)

    self:register("tauri:store:get", function(payload)
        self.security:assert_capability("store:get")
        return self.store:get(payload and payload.key)
    end)

    self:register("tauri:store:set", function(payload)
        self.security:assert_capability("store:set")
        return self.store:set(payload and payload.key, payload and payload.value)
    end)

    self:register("tauri:store:has", function(payload)
        self.security:assert_capability("store:get")
        return self.store:has(payload and payload.key)
    end)

    self:register("tauri:store:delete", function(payload)
        self.security:assert_capability("store:set")
        return self.store:delete(payload and payload.key)
    end)

    self:register("tauri:store:clear", function()
        self.security:assert_capability("store:set")
        return self.store:clear()
    end)

    local function parse_command_line(cmd_str)
        local tokens = {}
        local pos = 1
        local len = #cmd_str
        while pos <= len do
            local s, e = cmd_str:find("^%s+", pos)
            if s then pos = e + 1 end
            if pos > len then break end

            local char = cmd_str:sub(pos, pos)
            if char == '"' then
                local next_pos = cmd_str:find('"', pos + 1)
                if next_pos then
                    table.insert(tokens, cmd_str:sub(pos + 1, next_pos - 1))
                    pos = next_pos + 1
                else
                    table.insert(tokens, cmd_str:sub(pos + 1))
                    break
                end
            elseif char == "'" then
                local next_pos = cmd_str:find("'", pos + 1)
                if next_pos then
                    table.insert(tokens, cmd_str:sub(pos + 1, next_pos - 1))
                    pos = next_pos + 1
                else
                    table.insert(tokens, cmd_str:sub(pos + 1))
                    break
                end
            else
                local next_space = cmd_str:find("%s", pos)
                if next_space then
                    table.insert(tokens, cmd_str:sub(pos, next_space - 1))
                    pos = next_space
                else
                    table.insert(tokens, cmd_str:sub(pos))
                    break
                end
            end
        end
        return tokens
    end

    self:register("tauri:shell:execute", function(payload, caller, channel)
        local raw_cmd = payload.program or payload.command or payload.cmd
        if not raw_cmd or raw_cmd == "" then
            return { code = 1, error = "No command provided" }
        end

        local program = nil
        local args = {}

        if payload.args and type(payload.args) == "table" then
            program = tostring(raw_cmd)
            for _, a in ipairs(payload.args) do
                table.insert(args, tostring(a))
            end
        else
            local tokens = parse_command_line(tostring(raw_cmd))
            if #tokens == 0 then
                return { code = 1, error = "Empty command" }
            end
            program = tokens[1]
            for i = 2, #tokens do
                table.insert(args, tokens[i])
            end
        end

        self.security:assert_program(program)
        self.security:assert_args(program, args)
        local arg_count = #args
        local c_args = nil
        if arg_count > 0 then
            c_args = ffi.new("const char*[?]", arg_count)
            for i = 1, arg_count do
                c_args[i - 1] = args[i]
            end
        end

        local handle = clib.platform_process_spawn(program, c_args, arg_count)
        if handle == nil then
            return { code = 1, error = string.format("Failed to spawn process: %s", program) }
        end

        local ch = channel or (payload and payload.onProgress)
        local buf_size = 65536
        local buf = ffi.new("char[?]", buf_size)
        local lines = {}
        local total_bytes = 0
        local max_bytes = 1048576 -- 1MB output cap
        local max_lines = 10000   -- 10,000 lines cap
        local capped = false

        while true do
            local n = clib.platform_process_read_line(handle, buf, buf_size)
            if n <= 0 then
                break
            end
            local line = ffi.string(buf, n)
            total_bytes = total_bytes + n
            if #lines < max_lines and total_bytes <= max_bytes then
                table.insert(lines, line)
            elseif not capped then
                capped = true
                table.insert(lines, "[output truncated: limit exceeded]")
            end
            if ch then
                ch:send({ line = line, type = "stdout" })
            end
        end

        local exit_code = clib.platform_process_wait(handle)
        clib.platform_process_close(handle)

        return {
            code = exit_code,
            stdout = table.concat(lines, "\n")
        }
    end)

    self:register("tauri:event:emit", function(payload)
        self.security:assert_capability("event:emit")
        self:emit(payload and payload.event, payload and payload.payload)
        return true
    end)

    self:register("tauri:window:create", function(payload)
        self.security:assert_capability("core:default")
        payload = type(payload) == "table" and payload or {}
        local label = tostring(payload.label or ("win_" .. tostring(os.time())))
        if not label:match("^[a-zA-Z0-9_%-]+$") or #label > 64 then
            error("Invalid window label")
        end
        local title = tostring(payload.title or "New Window")
        local raw_url = tostring(payload.url or "")
        if raw_url:find("%z") or raw_url:find("[\r\n]") then
            error("Invalid window URL")
        end
        local lower_raw = raw_url:lower():gsub("^%s+", "")
        if lower_raw:match("^javascript:") or lower_raw:match("^data:") or lower_raw:match("^file:") or lower_raw:match("^vbscript:") or lower_raw:match("^blob:") or lower_raw:match("^//") then
            self.security:assert_capability("window:create:external")
            error("Forbidden URL scheme in tauri:window:create")
        end
        local url = raw_url
        if url == "" then
            url = self.default_origin .. "index.html"
        elseif not url:match("^https?://") and not url:match("^tauri://") then
            if url:find(":", 1, true) then
                self.security:assert_capability("window:create:external")
                error("Forbidden URL scheme in tauri:window:create")
            end
            url = self.default_origin .. url:gsub("^/+", "")
        end
        local is_internal = false
        if clib and clib.platform_is_origin_allowed then
            local ok_o, allowed = pcall(function() return clib.platform_is_origin_allowed(url) end)
            if ok_o and allowed == 1 then
                is_internal = true
            end
        else
            local low = url:lower()
            if (low == "tauri://localhost" or low:match("^tauri://localhost[/%?#]") or low == "https://tauri.localhost" or low:match("^https://tauri%.localhost[/%?#]")) and not low:find("@", 1, true) then
                is_internal = true
            end
        end
        if not is_internal then
            self.security:assert_capability("window:create:external")
        end
        local width = tonumber(payload.width) or 800
        local height = tonumber(payload.height) or 600
        local resizable = (payload.resizable == nil or payload.resizable) and 1 or 0
        local decorations = (payload.decorations == nil or payload.decorations) and 1 or 0
        local transparent = payload.transparent and 1 or 0
        if clib then
            local ok_c, rc = pcall(function()
                return clib.platform_window_create(label, title, url, width, height, resizable, decorations, transparent)
            end)
            if not ok_c or rc ~= 0 then
                error("Impossibile creare la finestra: " .. label)
            end
        end
        self.windows[label] = { label = label, title = title, url = url }
        return { label = label }
    end)

    local function resolve_win_label(payload, caller)
        local lbl = (type(payload) == "table" and payload.label) or caller or "main"
        lbl = tostring(lbl)
        if lbl == "" then lbl = "main" end
        return lbl
    end

    self:register("tauri:window:close", function(payload, caller)
        self.security:assert_capability("core:default")
        local target = resolve_win_label(payload, caller)
        clib.platform_window_close(target)
        return true
    end)

    self:register("tauri:window:destroy", function(payload, caller)
        self.security:assert_capability("core:default")
        local target = resolve_win_label(payload, caller)
        clib.platform_window_force_close(target)
        self.windows[target] = nil
        return true
    end)

    self:register("tauri:window:force_close", function(payload, caller)
        self.security:assert_capability("core:default")
        local target = resolve_win_label(payload, caller)
        clib.platform_window_force_close(target)
        self.windows[target] = nil
        return true
    end)

    self:register("tauri:window:set_title", function(payload, caller)
        self.security:assert_capability("core:default")
        local target = resolve_win_label(payload, caller)
        clib.platform_window_set_title(target, tostring((payload and payload.title) or ""))
        return true
    end)

    self:register("tauri:window:set_size", function(payload, caller)
        self.security:assert_capability("core:default")
        local target = resolve_win_label(payload, caller)
        clib.platform_window_set_size(target, tonumber(payload and payload.width) or 800, tonumber(payload and payload.height) or 600)
        return true
    end)

    self:register("tauri:window:minimize", function(payload, caller)
        self.security:assert_capability("core:default")
        local target = resolve_win_label(payload, caller)
        clib.platform_window_minimize(target)
        return true
    end)

    self:register("tauri:window:maximize", function(payload, caller)
        self.security:assert_capability("core:default")
        local target = resolve_win_label(payload, caller)
        clib.platform_window_maximize(target)
        return true
    end)

    self:register("tauri:window:show", function(payload, caller)
        self.security:assert_capability("core:default")
        local target = resolve_win_label(payload, caller)
        clib.platform_window_show(target)
        return true
    end)

    self:register("tauri:window:hide", function(payload, caller)
        self.security:assert_capability("core:default")
        local target = resolve_win_label(payload, caller)
        clib.platform_window_hide(target)
        return true
    end)

    self:register("tauri:window:set_focus", function(payload, caller)
        self.security:assert_capability("core:default")
        local target = resolve_win_label(payload, caller)
        clib.platform_window_set_focus(target)
        return true
    end)

    self:register("tauri:window:start_dragging", function(payload, caller)
        self.security:assert_capability("core:default")
        local target = resolve_win_label(payload, caller)
        clib.platform_window_start_dragging(target)
        return true
    end)

    self:register("tauri:tray:create", function(payload)
        self.security:assert_capability("core:default")
        local ok = clib.platform_tray_create(payload.tooltip or "Tauri App", payload.icon or "")
        return ok == 1
    end)

    self:register("tauri:shortcut:register", function(payload)
        self.security:assert_capability("core:default")
        local ok = clib.platform_shortcut_register(payload.shortcut or "")
        return ok == 1
    end)

    self:register("tauri:shortcut:unregister", function(payload)
        self.security:assert_capability("core:default")
        clib.platform_shortcut_unregister(payload.shortcut or "")
        return true
    end)

    self:register("tauri:updater:check", function()
        error("tauri:updater:check non è implementato: updater disabilitato per motivi di sicurezza")
    end)

    self:register("tauri:updater:install", function()
        error("tauri:updater:install non è implementato: updater disabilitato per motivi di sicurezza")
    end)

    self:register("tauri:fs:read_text_file", function(payload)
        self.security:assert_capability("fs:read-text-file")
        local safe_path = self.security:assert_scope(payload and payload.path, self.base_paths)
        local f, err = io.open(safe_path, "rb")
        if not f then
            error("Impossibile leggere il file: " .. tostring(err))
        end
        local size = f:seek("end")
        if size and size > 52428800 then
            f:close()
            error("File exceeds maximum read_text_file size limit (50MB)")
        end
        f:seek("set", 0)
        local content = f:read("*a")
        f:close()
        return content
    end)

    self:register("tauri:fs:write_text_file", function(payload)
        self.security:assert_capability("fs:write-text-file")
        local safe_path = self.security:assert_scope(payload.path, self.base_paths)
        local f, err = io.open(safe_path, "w")
        if not f then error(err) end
        f:write(tostring(payload.contents or ""))
        f:close()
        return true
    end)

    self:register("tauri:fs:read_dir", function(payload)
        self.security:assert_capability("fs:read-dir")
        local safe_path = self.security:assert_scope(payload.path, self.base_paths)
        local entries = {}
        if ffi.os == "Windows" then
            local search_path = safe_path:gsub("[/\\]+$", "") .. "\\*"
            local k32 = clib
            pcall(function() k32 = ffi.load("kernel32") end)
            local mb2wc = (k32 and k32.MultiByteToWideChar) or clib.MultiByteToWideChar
            local wc2mb = (k32 and k32.WideCharToMultiByte) or clib.WideCharToMultiByte
            local fn_first = (k32 and k32.FindFirstFileW) or clib.FindFirstFileW
            local fn_next = (k32 and k32.FindNextFileW) or clib.FindNextFileW
            local fn_close = (k32 and k32.FindClose) or clib.FindClose

            local wlen = mb2wc(65001, 0, search_path, #search_path, nil, 0)
            local wsearch = ffi.new("uint16_t[?]", wlen + 1)
            mb2wc(65001, 0, search_path, #search_path, wsearch, wlen)
            wsearch[wlen] = 0

            local find_data = ffi.new("WIN32_FIND_DATAW")
            local hFind = fn_first(wsearch, find_data)
            if hFind ~= ffi.cast("void*", -1) and hFind ~= nil then
                while true do
                    local nlen = 0
                    while find_data.cFileName[nlen] ~= 0 do nlen = nlen + 1 end
                    local ulen = wc2mb(65001, 0, find_data.cFileName, nlen, nil, 0, nil, nil)
                    local ubuf = ffi.new("char[?]", ulen + 1)
                    wc2mb(65001, 0, find_data.cFileName, nlen, ubuf, ulen, nil, nil)
                    ubuf[ulen] = 0
                    local name = ffi.string(ubuf, ulen)
                    if name ~= "." and name ~= ".." then
                        local is_dir = (bit.band(find_data.dwFileAttributes, 0x10) ~= 0)
                        table.insert(entries, { name = name, isDirectory = is_dir })
                    end
                    if fn_next(hFind, find_data) == 0 then
                        break
                    end
                end
                fn_close(hFind)
            else
                error("Impossibile aprire la directory: " .. payload.path)
            end
        else
            local dir = clib.opendir(safe_path)
            if dir == nil then error("Impossibile aprire la directory: " .. payload.path) end
            while true do
                local entry = clib.readdir(dir)
                if entry == nil then break end
                local name = ffi.string(entry.d_name)
                if name ~= "." and name ~= ".." then
                    table.insert(entries, { name = name, isDirectory = (entry.d_type == 4) })
                end
            end
            clib.closedir(dir)
        end
        return entries
    end)

    self:register("tauri:fs:remove", function(payload)
        self.security:assert_capability("fs:remove")
        local safe_path = self.security:assert_scope(payload.path, self.base_paths)
        if clib.remove(safe_path) ~= 0 then
            error("Impossibile eliminare: " .. payload.path)
        end
        return true
    end)

    self:register("tauri:dialog:open", function(payload)
        self.security:assert_capability("dialog:open")
        local title = payload and payload.title or nil
        local buf = ffi.new("char[1024]")
        local ok = clib.platform_dialog_open(title, nil, buf, 1024)
        return ok == 1 and ffi.string(buf) or nil
    end)

    self:register("tauri:dialog:save", function(payload)
        self.security:assert_capability("dialog:save")
        local title = payload and payload.title or nil
        local buf = ffi.new("char[1024]")
        local ok = clib.platform_dialog_save(title, nil, buf, 1024)
        return ok == 1 and ffi.string(buf) or nil
    end)

    self:register("tauri:dialog:message", function(payload)
        self.security:assert_capability("dialog:message")
        clib.platform_dialog_message(payload.title or "Message", payload.message or "", 0)
        return true
    end)

    self:register("tauri:dialog:ask", function(payload)
        self.security:assert_capability("dialog:message")
        return clib.platform_dialog_message(payload.title or "Question", payload.message or "", 1) == 1
    end)

    self:register("tauri:clipboard:write_text", function(payload)
        self.security:assert_capability("clipboard:write-text")
        return self.clipboard.write_text(payload.text or "")
    end)

    self:register("tauri:clipboard:read_text", function()
        self.security:assert_capability("clipboard:read-text")
        return self.clipboard.read_text()
    end)

    self:register("tauri:clipboard:write_html", function(payload)
        self.security:assert_capability("clipboard:write-text")
        return self.clipboard.write_html(payload.html or "")
    end)

    self:register("tauri:clipboard:read_html", function()
        self.security:assert_capability("clipboard:read-text")
        return self.clipboard.read_html()
    end)

    self:register("tauri:clipboard:write_image", function(payload)
        self.security:assert_capability("clipboard:write-text")
        return self.clipboard.write_image(payload.image or payload.data or "")
    end)

    self:register("tauri:clipboard:read_image", function()
        self.security:assert_capability("clipboard:read-text")
        return self.clipboard.read_image()
    end)

    self:register("tauri:clipboard:clear", function()
        self.security:assert_capability("clipboard:write-text")
        self.clipboard.clear()
        return true
    end)

    self:register("tauri:theme:get", function()
        self.security:assert_capability("theme:get")
        return self:get_theme()
    end)

    self:register("tauri:notification:send", function(payload)
        self.security:assert_capability("notification:send")
        clib.platform_notification_send(payload and payload.title or "App", payload and payload.body or "")
        return true
    end)

    self:register("tauri:path:app_data_dir", function()
        self.security:assert_capability("path:read")
        return self.base_paths.appData
    end)
    self:register("tauri:path:home_dir", function()
        self.security:assert_capability("path:read")
        return self.base_paths.home
    end)
    self:register("tauri:path:desktop_dir", function()
        self.security:assert_capability("path:read")
        return self.base_paths.desktop
    end)

    self:register("tauri:menu:set", function(payload)
        self.security:assert_capability("core:default")
        if payload and payload.menu then
            return self:set_menu(payload.menu)
        end
        return false
    end)
end

function Tauri:register(cmd_name, handler)
    self.commands[cmd_name] = handler
end

function Tauri:plugin(plugin)
    if type(plugin) == "function" then
        plugin = plugin(self)
    end
    if type(plugin) ~= "table" then
        error("app:plugin expects a plugin table or a factory function returning a table")
    end

    local name = plugin.name or string.format("plugin_%d", #self.plugin_list + 1)
    if self.plugins[name] then
        error(string.format("Plugin '%s' is already registered", name))
    end

    plugin.app = self
    self.plugins[name] = plugin
    table.insert(self.plugin_list, plugin)

    if type(plugin.setup) == "function" then
        local ok, err = pcall(plugin.setup, self, plugin)
        if not ok then
            error(string.format("Failed to initialize plugin '%s': %s", name, tostring(err)))
        end
    end

    return self
end

function Tauri:listen(event_name, handler)
    if not self.listeners[event_name] then
        self.listeners[event_name] = {}
    end
    table.insert(self.listeners[event_name], handler)
end

function Tauri:emit(event_name, payload)
    if self.listeners and self.listeners[event_name] then
        for _, cb in ipairs(self.listeners[event_name]) do
            pcall(cb, payload)
        end
    end
    local js = string.format(
        "if(window.__TAURI__&&window.__TAURI__.event){window.__TAURI__.event._dispatch(%s,%s);}",
        json.encode(event_name),
        json.encode(payload)
    )
    if clib and clib.platform_window_eval then
        pcall(clib.platform_window_eval, "*", js)
    end
end

function _G.__TAURI_DISPATCH_EMIT__(event_name, payload_json)
    if not g_tauri_instance then return end
    local payload = {}
    if payload_json and payload_json ~= "" then
        local ok, decoded = pcall(json.decode, payload_json)
        if ok and decoded ~= nil then
            payload = decoded
        end
    end
    g_tauri_instance:emit(event_name, payload)
end

function Tauri:create_main_window()
    if __IS_ISOLATE_WORKER__ or self._main_window_created then
        return
    end
    self._main_window_created = true

    local win_conf = {}
    if self.config.app and self.config.app.windows and self.config.app.windows[1] then
        win_conf = self.config.app.windows[1]
    elseif self.config.tauri and self.config.tauri.windows and self.config.tauri.windows[1] then
        win_conf = self.config.tauri.windows[1]
    elseif self.config.tauri and self.config.tauri.window then
        win_conf = self.config.tauri.window
    end

    local title = win_conf.title or (self.config.package and self.config.package.name) or self.config.productName or "Tauri App"
    local width = win_conf.width or 960
    local height = win_conf.height or 640
    local resizable = win_conf.resizable ~= false and 1 or 0
    local decorations = win_conf.decorations ~= false and 1 or 0
    local transparent = win_conf.transparent and 1 or 0

    local url = win_conf.url
    if not url or url == "" then
        url = self.default_origin
    elseif not url:match("^https?://") and not url:match("^tauri://") then
        if self.default_origin:match("/$") then
            url = self.default_origin .. url:gsub("^/", "")
        else
            url = self.default_origin .. "/" .. url:gsub("^/", "")
        end
    end

    local is_trusted = is_trusted_origin(url)
    pcall(function() clib.platform_set_client_script(TAURI_CLIENT_SCRIPT) end)
    clib.platform_window_create("main", title, url, width, height, resizable, decorations, transparent)
    if is_trusted then
        pcall(function() clib.platform_window_init_script("main", TAURI_CLIENT_SCRIPT) end)
        clib.platform_window_eval("main", TAURI_CLIENT_SCRIPT)
        for _, p in ipairs(self.plugin_list or {}) do
            if type(p.js) == "string" and p.js ~= "" then
                clib.platform_window_eval("main", p.js)
            end
        end
    end
    self.windows["main"] = true
end

function Tauri:run()
    if not self._main_window_created and not __IS_ISOLATE_WORKER__ then
        self:create_main_window()
    end
end

function __TAURI_DISPATCH_IPC__(label, seq, req)
    if not g_tauri_instance then return end
    local parsed = json.decode(req)
    if type(parsed) ~= "table" then
        clib.platform_window_return_async(label, seq, 1, json.encode({ error = "Bad IPC Envelope" }))
        return
    end

    local cmd = parsed[1]
    local raw_args = parsed[2]
    local args = type(raw_args) == "string" and json.decode(raw_args) or raw_args

    local handler = g_tauri_instance.commands[cmd]
    if not handler then
        clib.platform_window_return_async(label, seq, 1, json.encode({ error = "Command not found: " .. tostring(cmd) }))
        return
    end

    local primary_channel = nil
    local function wrap_channel(channel_id)
        local ch = { id = channel_id, label = label }
        function ch:send(data)
            local js = string.format("if(window.__TAURI_DISPATCH_CHANNEL__){window.__TAURI_DISPATCH_CHANNEL__(%s,%s);}",
                json.encode(self.id),
                json.encode(data))
            clib.platform_window_eval(self.label or "*", js)
        end
        function ch:close()
            local js = string.format("if(window.__TAURI_CHANNELS__&&window.__TAURI_CHANNELS__[%s]){delete window.__TAURI_CHANNELS__[%s];}",
                json.encode(self.id), json.encode(self.id))
            clib.platform_window_eval(self.label or "*", js)
        end
        return ch
    end

    if type(args) == "table" then
        if args.__tauri_channel__ then
            primary_channel = wrap_channel(args.__tauri_channel__)
            args = primary_channel
        else
            for k, v in pairs(args) do
                if type(v) == "table" and v.__tauri_channel__ then
                    local ch = wrap_channel(v.__tauri_channel__)
                    args[k] = ch
                    if not primary_channel then
                        primary_channel = ch
                    end
                end
            end
        end
    end

    local ok, res = pcall(handler, args, label, primary_channel)
    if ok then
        local return_payload
        if res == nil then
            return_payload = '{"data":null}'
        else
            return_payload = json.encode({ data = res })
        end
        clib.platform_window_return_async(label, seq, 0, return_payload)
    else
        clib.platform_window_return_async(label, seq, 1, json.encode({ error = tostring(res) }))
    end
end

function __TAURI_DISPATCH_TRAY__(id)
    if not g_tauri_instance then return end
    if id == 1 then
        clib.platform_window_show("main")
    elseif id == 2 then
        clib.bridge_terminate()
    end
end

function __TAURI_DISPATCH_SHORTCUT__(shortcut)
    if not g_tauri_instance then return end
    g_tauri_instance:emit("tauri://shortcut-" .. shortcut, {})
end

function __TAURI_DISPATCH_DEEPLINK__(url)
    if not g_tauri_instance then return end
    g_tauri_instance:emit("tauri://deep-link", { url = url })
end

function __TAURI_DISPATCH_MENU__(id)
    if not g_tauri_instance then return end
    local event_data = { id = id }
    for _, cb in ipairs(g_tauri_instance.menu_listeners or {}) do
        pcall(cb, event_data)
    end
    g_tauri_instance:emit("tauri://menu-event", event_data)
end

function __TAURI_DISPATCH_WINDOW_CLOSE__(label)
    if not g_tauri_instance then return 0 end
    local event = {
        event = "close-requested",
        label = label or "main",
        _prevented = false,
        prevent_default = function(self)
            self._prevented = true
        end
    }

    for _, p in ipairs(g_tauri_instance.plugin_list or {}) do
        if type(p.on_window_close) == "function" then
            pcall(p.on_window_close, g_tauri_instance, event)
        end
        if type(p.on_event) == "function" then
            pcall(p.on_event, g_tauri_instance, "window_close", event)
        end
    end

    for _, cb in ipairs(g_tauri_instance.window_listeners or {}) do
        pcall(cb, event)
    end

    g_tauri_instance:emit("tauri://close-requested", { label = event.label })
    if not event._prevented and g_tauri_instance and g_tauri_instance.sidecars then
        for _, sc in ipairs(g_tauri_instance.sidecars) do
            if sc.auto_kill ~= false then
                pcall(function() sc:kill() end)
            end
        end
    end


    return event._prevented and 1 or 0
end

local MAX_SINGLE_INSTANCE_PAYLOAD = 65536

local function validate_single_instance_payload(payload_or_size)
    local sz = type(payload_or_size) == "number" and payload_or_size or #(tostring(payload_or_size or ""))
    if sz > MAX_SINGLE_INSTANCE_PAYLOAD then
        error("Payload exceeds maximum single-instance size limit")
    end
    return true
end

local function resolve_single_instance_socket_path(app_name)
    local safe_id = tostring(app_name or "tauri-app"):gsub("[^a-zA-Z0-9_%-%.]", "_")
    local xdg = os.getenv("XDG_RUNTIME_DIR")
    if xdg and xdg ~= "" then
        return xdg .. "/" .. safe_id .. ".sock"
    end
    local home = os.getenv("HOME")
    if home and home ~= "" then
        return home .. "/.local/run/" .. safe_id .. ".sock"
    end
    error("No secure user runtime directory found (refusing insecure /tmp fallback)")
end

local function parse_and_normalize_url(raw_url)
    if type(raw_url) ~= "string" or raw_url == "" or raw_url:find("%z") then
        error("Invalid URL")
    end
    local scheme, authority, raw_path = raw_url:match("^([a-zA-Z][a-zA-Z0-9%+%-%.]*)://([^/%?#]+)(.*)$")
    if not scheme or not authority then
        error("Invalid URL")
    end
    if authority:find("@", 1, true) then
        error("Invalid URL: userinfo is forbidden")
    end
    scheme = scheme:lower()
    local host, port_str
    if authority:sub(1, 1) == "[" then
        host, port_str = authority:match("^%[([^%]]+)%]:?(%d*)$")
        if not host then
            error("Invalid IPv6 URL authority")
        end
        host = "[" .. host:lower() .. "]"
    else
        host, port_str = authority:match("^([^:]+):?(%d*)$")
        if not host or host:find(":", 1, true) then
            error("Invalid URL authority")
        end
        host = host:lower()
    end
    host = host:gsub("%.$", "")
    local path_only = (raw_path or ""):match("^([^%?#]*)") or ""
    if path_only == "" then path_only = "/" end
    local decoded_path = path_only:gsub("%%2[eE]", "."):gsub("%%2[fF]", "/"):gsub("%%5[cC]", "/")
    if decoded_path:find("%%00") then
        error("Invalid URL: NUL byte in path")
    end
    local segments = {}
    for seg in decoded_path:gmatch("[^/]+") do
        if seg == ".." then
            table.remove(segments)
        elseif seg ~= "." then
            table.insert(segments, seg)
        end
    end
    local norm_path = "/" .. table.concat(segments, "/")
    return {
        scheme = scheme,
        host = host,
        port = port_str ~= "" and tonumber(port_str) or nil,
        path = norm_path
    }
end

local function is_ssrf_forbidden(raw_host)
    if type(raw_host) ~= "string" or raw_host == "" then
        return true
    end
    local h = raw_host:lower():gsub("^%s+", ""):gsub("%s+$", "")
    if h:sub(1, 1) == "[" then
        h = h:match("^%[([^%]]+)%]") or h
    else
        h = h:match("^([^:]+)") or h
    end
    h = h:gsub("%.$", "")
    if h == "" or h == "localhost" or h:match("%.localhost$") or h:match("%.local$") or h == "metadata.google.internal" or h == "169.254.169.254" or h == "100.100.100.200" then
        return true
    end
    if h == "::1" or h == "::" or h == "0:0:0:0:0:0:0:1" or h == "0:0:0:0:0:0:0:0" then
        return true
    end
    if h:match("^::ffff:") or h:match("^fe[89ab][0-9a-f]:") or h:match("^f[cd][0-9a-f][0-9a-f]:") then
        return true
    end
    if h:match("^0[xX][0-9a-f]+$") or h:match("^%d+$") then
        return true
    end
    local o1, o2, o3, o4 = h:match("^(%d+)%.(%d+)%.(%d+)%.(%d+)$")
    if o1 then
        if o1:match("^0%d") or o2:match("^0%d") or o3:match("^0%d") or o4:match("^0%d") then
            return true
        end
        local a, b = tonumber(o1), tonumber(o2)
        local c, d = tonumber(o3), tonumber(o4)
        if not a or not b or not c or not d or a > 255 or b > 255 or c > 255 or d > 255 then
            return true
        end
        if a == 0 or a == 10 or a == 127 then
            return true
        end
        if a == 169 and b == 254 then
            return true
        end
        if a == 172 and b >= 16 and b <= 31 then
            return true
        end
        if a == 192 and b == 168 then
            return true
        end
        if a == 100 and b >= 64 and b <= 127 then
            return true
        end
    end
    return false
end

local function mask_db_uri(uri)
    if type(uri) ~= "string" then return uri end
    return uri:gsub("(%w+://)([^:]+):([^@]+)@", "%1%2:***@")
end

local function is_db_allowed(uri, allowed_scopes)
    if type(uri) ~= "string" or uri == "" or uri:find("%z") then
        return false
    end
    if type(allowed_scopes) ~= "table" or #allowed_scopes == 0 then
        return false
    end
    local lower_uri = uri:lower()
    if lower_uri:find("%%2e") or lower_uri:find("%%2f") or lower_uri:find("%%5c") or lower_uri:find("%%00") then
        return false
    end
    if lower_uri:match("^sqlite:") then
        local db_path = uri:sub(8)
        if db_path == "" or db_path:find("%.%.") or db_path:find("\\") or db_path:match("^/") or db_path:match("^[a-zA-Z]:") then
            return false
        end
    end
    for _, pattern in ipairs(allowed_scopes) do
        if type(pattern) == "string" and pattern ~= "" then
            local pat = pattern:gsub("([%(%)%.%%%+%-%?%[%^%$])", "%%%1"):gsub("%*", ".*")
            if uri:match("^" .. pat .. "$") then
                return true
            end
        end
    end
    return false
end

function __TAURI_DISPATCH_SINGLE_INSTANCE__(payload_json)
    if not g_tauri_instance then return end
    if type(payload_json) == "string" then
        validate_single_instance_payload(#payload_json)
    end
    local data = json.decode(payload_json) or {}
    for _, p in ipairs(g_tauri_instance.plugin_list or {}) do
        if type(p.on_single_instance) == "function" then
            pcall(p.on_single_instance, g_tauri_instance, data)
        end
        if type(p.on_event) == "function" then
            pcall(p.on_event, g_tauri_instance, "single_instance", data)
        end
    end
    for _, cb in ipairs(g_tauri_instance.single_instance_listeners or {}) do
        pcall(cb, data)
    end
    g_tauri_instance:emit("tauri://single-instance", data)
end

function __TAURI_DISPATCH_THEME__(theme)
    if not g_tauri_instance then return end
    g_tauri_instance._current_theme = theme
    for _, p in ipairs(g_tauri_instance.plugin_list or {}) do
        if type(p.on_theme_changed) == "function" then
            pcall(p.on_theme_changed, g_tauri_instance, theme)
        end
        if type(p.on_event) == "function" then
            pcall(p.on_event, g_tauri_instance, "theme_changed", { theme = theme })
        end
    end
    for _, cb in ipairs(g_tauri_instance.theme_listeners or {}) do
        pcall(cb, theme)
    end
    g_tauri_instance:emit("tauri://theme-changed", { theme = theme })
end

function __TAURI_DISPATCH_FILE_DROP__(label, event_type, payload_json)
    if not g_tauri_instance then return end
    local data = json.decode(payload_json) or {}
    data.label = label
    data.type = event_type
    for _, p in ipairs(g_tauri_instance.plugin_list or {}) do
        if type(p.on_file_drop) == "function" then
            pcall(p.on_file_drop, g_tauri_instance, data)
        end
        if type(p.on_event) == "function" then
            pcall(p.on_event, g_tauri_instance, "file_drop", data)
        end
    end
    for _, cb in ipairs(g_tauri_instance.file_drop_listeners or {}) do
        pcall(cb, data)
    end
    if event_type == "drop" then
        g_tauri_instance:emit("tauri://drag-drop", data)
    elseif event_type == "hover" then
        g_tauri_instance:emit("tauri://drag-over", data)
    elseif event_type == "cancel" then
        g_tauri_instance:emit("tauri://drag-leave", data)
    end
end

Tauri.Menu = Menu
Tauri.Sidecar = Sidecar
Tauri.spawn_sidecar = Sidecar.spawn
Tauri.Submenu = Submenu
Tauri.MenuItem = MenuItem
Tauri.CheckMenuItem = CheckMenuItem
Tauri.PredefinedMenuItem = PredefinedMenuItem
Tauri.validate_single_instance_payload = validate_single_instance_payload
Tauri.resolve_single_instance_socket_path = resolve_single_instance_socket_path
Tauri.resolve_safe_socket_path = resolve_single_instance_socket_path
Tauri.parse_and_normalize_url = parse_and_normalize_url
Tauri.is_ssrf_forbidden = is_ssrf_forbidden
Tauri.mask_db_uri = mask_db_uri
Tauri.is_db_allowed = is_db_allowed

return {
    Tauri = Tauri,
    init = Tauri.new,
    get_instance = function() return g_tauri_instance end,
    get_app = function() return g_tauri_instance end,
    new = Tauri.new,
    sleep = Tauri.sleep,
    json = json,
    _filter_absolute_only = filter_absolute_only,
    validate_single_instance_payload = validate_single_instance_payload,
    resolve_single_instance_socket_path = resolve_single_instance_socket_path,
    resolve_safe_socket_path = resolve_single_instance_socket_path,
    parse_and_normalize_url = parse_and_normalize_url,
    is_ssrf_forbidden = is_ssrf_forbidden,
    mask_db_uri = mask_db_uri,
    is_db_allowed = is_db_allowed,
    notification = {
        send = function(title, body)
            if type(title) == "table" then
                body = title.body or title.message or ""
                title = title.title or "App"
            end
            clib.platform_notification_send(tostring(title or "App"), tostring(body or ""))
        end
    },
    plugin = function(name, def)
        if type(name) == "table" and not def then
            def = name
            name = def.name or "anonymous"
        end
        def = def or {}
        def.name = name
        return def
    end,
    Sidecar = Sidecar,
    spawn_sidecar = function(name, opts)
        return Sidecar.spawn(name, opts, g_tauri_instance)
    end,
    Menu = Menu,
    Submenu = Submenu,
    MenuItem = MenuItem,
    CheckMenuItem = CheckMenuItem,
    PredefinedMenuItem = PredefinedMenuItem
}