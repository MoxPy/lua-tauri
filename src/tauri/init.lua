local ffi = require("ffi")
local bit = bit or require("bit")

-- ============================================================================
-- Module & Plugin Search Paths (Auto-Discovery)
-- In RELEASE builds only absolute exec-relative paths are added.
-- Relative cwd paths (./  ../) are restricted to DEV mode only, activated
-- by setting the TAURILUA_DEV=1 environment variable (set by `lua-tauri dev`).
-- ============================================================================
local function setup_module_search_paths()
    -- Detect dev mode via env variable (set before Lua state is created).
    -- __TAURI_DEV__ is injected later by the C host, so we cannot use it here.
    local function is_dev_mode()
        -- Check env via os.getenv (available before ffi.cdef redeclaration)
        local ok, val = pcall(os.getenv, "TAURILUA_DEV")
        if ok and val and val ~= "" and val ~= "0" then
            return true
        end
        return false
    end

    local dev_mode = is_dev_mode()

    local function get_executable_dir()
        local dir = nil
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
                        local full_path = ffi.string(ubuf, ulen):gsub("\\", "/")
                        dir = full_path:match("^(.*)/[^/]+$")
                    end
                end
            end
        end)
        return dir
    end

    local exec_dir = get_executable_dir()

    -- Paths that are safe in both dev and release (absolute, exec-relative)
    local extra_lua_paths = {}
    local extra_c_paths = {}

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
            -- macOS App Bundle (.app/Contents/Resources/rocks)
            local res_dir = exec_dir:gsub("/MacOS$", "/Resources")
            table.insert(extra_lua_paths, res_dir .. "/rocks/share/lua/5.1/?.lua")
            table.insert(extra_lua_paths, res_dir .. "/rocks/share/lua/5.1/?/init.lua")
            table.insert(extra_c_paths,   res_dir .. "/rocks/lib/lua/5.1/?.so")
            table.insert(extra_c_paths,   res_dir .. "/rocks/lib/lua/5.1/?.dylib")
        else
            -- Linux / Windows standalone or installed packages
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
        end
    end

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

    local current_path = package.path or ""
    for _, p in ipairs(extra_lua_paths) do
        if not current_path:find(p, 1, true) then
            current_path = current_path .. ";" .. p
        end
    end
    if not dev_mode then
        current_path = filter_absolute_only(current_path)
    end
    package.path = current_path

    local current_cpath = package.cpath or ""
    for _, cp in ipairs(extra_c_paths) do
        if not current_cpath:find(cp, 1, true) then
            current_cpath = current_cpath .. ";" .. cp
        end
    end
    if not dev_mode then
        current_cpath = filter_absolute_only(current_cpath)
    end
    package.cpath = current_cpath
end

setup_module_search_paths()

ffi.cdef[[
typedef void (*ipc_callback_fn)(const char *window_label, const char *seq, const char *req);
typedef void (*tray_callback_fn)(int id);
typedef void (*shortcut_callback_fn)(const char *shortcut);
typedef void (*deeplink_callback_fn)(const char *url);

void bridge_init(ipc_callback_fn on_ipc, tray_callback_fn on_tray, shortcut_callback_fn on_shortcut, deeplink_callback_fn on_deeplink);
void bridge_run_loop();
void bridge_terminate();

void platform_set_assets_path(const char *path);

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
void platform_window_eval(const char *label, const char *js);
void platform_window_return_async(const char *label, const char *seq, int status, const char *result);

int platform_tray_create(const char *tooltip, const char *icon_path);
void platform_tray_set_menu(const char *json_menu);
void platform_tray_destroy();
void platform_set_menu(const char *json_menu);

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
]]

local clib = ffi.C

local json = {}

local function escape_str(s)
    local map = {
        ['\\'] = '\\\\',
        ['"'] = '\\"',
        ['\b'] = '\\b',
        ['\f'] = '\\f',
        ['\n'] = '\\n',
        ['\r'] = '\\r',
        ['\t'] = '\\t'
    }
    return s:gsub('[\\"\b\f\n\r\t]', map)
end

function json.encode(val)
    local t = type(val)
    if t == "nil" then
        return "null"
    elseif t == "boolean" then
        return val and "true" or "false"
    elseif t == "number" then
        return tostring(val)
    elseif t == "string" then
        return '"' .. escape_str(val) .. '"'
    elseif t == "table" then
        local is_array = true
        local max_idx = 0
        for k, v in pairs(val) do
            if type(k) == "number" and k > 0 and math.floor(k) == k then
                if k > max_idx then max_idx = k end
            else
                is_array = false
                break
            end
        end
        if is_array then
            local items = {}
            for i = 1, max_idx do
                items[i] = json.encode(val[i])
            end
            return "[" .. table.concat(items, ",") .. "]"
        else
            local fields = {}
            for k, v in pairs(val) do
                table.insert(fields, string.format("%q:%s", tostring(k), json.encode(v)))
            end
            return "{" .. table.concat(fields, ",") .. "}"
        end
    end
    return "null"
end

local function parse_value(str, idx)
    idx = str:find("%S", idx) or idx
    local char = str:sub(idx, idx)

    if char == "{" then
        local obj = {}
        idx = idx + 1
        while true do
            idx = str:find("%S", idx) or idx
            if str:sub(idx, idx) == "}" then return obj, idx + 1 end
            if str:sub(idx, idx) == "," then idx = idx + 1 end
            idx = str:find("%S", idx) or idx
            if str:sub(idx, idx) == "}" then return obj, idx + 1 end
            
            local key, next_idx = parse_value(str, idx)
            idx = str:find("%S", next_idx) or next_idx
            if str:sub(idx, idx) == ":" then idx = idx + 1 end
            local val
            val, idx = parse_value(str, idx)
            obj[key] = val
        end
    elseif char == "[" then
        local arr = {}
        idx = idx + 1
        while true do
            idx = str:find("%S", idx) or idx
            if str:sub(idx, idx) == "]" then return arr, idx + 1 end
            if str:sub(idx, idx) == "," then idx = idx + 1 end
            idx = str:find("%S", idx) or idx
            if str:sub(idx, idx) == "]" then return arr, idx + 1 end
            local val
            val, idx = parse_value(str, idx)
            table.insert(arr, val)
        end
    elseif char == '"' then
        local end_idx = idx + 1
        while true do
            end_idx = str:find('"', end_idx)
            if not end_idx then return "", #str + 1 end
            local backslashes = 0
            local b_idx = end_idx - 1
            while b_idx >= idx and str:sub(b_idx, b_idx) == "\\" do
                backslashes = backslashes + 1
                b_idx = b_idx - 1
            end
            if backslashes % 2 == 0 then break end
            end_idx = end_idx + 1
        end
        local val = str:sub(idx + 1, end_idx - 1)
        val = val:gsub('\\"', '"'):gsub('\\\\', '\\'):gsub('\\n', '\n'):gsub('\\r', '\r'):gsub('\\t', '\t')
        return val, end_idx + 1
    elseif str:sub(idx, idx + 3) == "true" then
        return true, idx + 4
    elseif str:sub(idx, idx + 4) == "false" then
        return false, idx + 5
    elseif str:sub(idx, idx + 3) == "null" then
        return nil, idx + 4
    else
        local next_delim = str:find("[,%]}%s]", idx) or (#str + 1)
        local raw = str:sub(idx, next_delim - 1)
        return tonumber(raw) or raw, next_delim
    end
end

function json.decode(str)
    if not str or str == "" then return nil end
    return parse_value(str, 1)
end

local Security = {}
Security.__index = Security

function Security.new(config)
    local self = setmetatable({}, Security)
    local sec = (config.tauri and config.tauri.security) or config.security or {}
    self.capabilities = {}
    if sec.capabilities then
        for _, cap in ipairs(sec.capabilities) do
            self.capabilities[cap] = true
        end
    end
    self.scopes = sec.scope or {}

    -- allowed_programs: list of strings or tables {name="cmd", args={"--allowed-flag", ...}}
    -- String entry means: program allowed (subject to dangerous flag blocking).
    -- Table entry means: program allowed ONLY with args from the given list.
    self.allowed_programs = sec.allowed_programs or sec.allowed_commands or {}
    -- Build a fast-lookup map: program_name -> arg_whitelist (false = any args ok, table = strict args)
    self._program_map = {}
    for _, entry in ipairs(self.allowed_programs) do
        if type(entry) == "string" then
            if entry == "*" then
                self._program_map["*"] = false  -- wildcard: any program
            else
                self._program_map[entry] = false  -- specific program
            end
        elseif type(entry) == "table" then
            local prog = entry.name or entry.program or entry.cmd
            if prog then
                self._program_map[prog] = entry.args or false
            end
        end
    end
    return self
end

function Security:assert_capability(name)
    if not self.capabilities[name] then
        error(string.format("Capability non autorizzata: %s", name))
    end
end

function Security:assert_program(program)
    if not self.capabilities["shell:execute"] and not self.capabilities["shell:allow-execute"] then
        error("Capability non autorizzata: shell:execute")
    end

    -- Windows .bat/.cmd injection protection: reject shell script wrappers
    local prog_lower = tostring(program):lower()
    if prog_lower:match("%.bat$") or prog_lower:match("%.cmd$") then
        error(string.format("Shell: direct execution of .bat / .cmd files is blocked for security (CWE-78): %s", program))
    end

    -- Default-deny: allowed_programs is mandatory (empty list = nothing allowed)
    if not self.allowed_programs or #self.allowed_programs == 0 then
        error(string.format("Shell: execution denied (allowed_programs is empty or missing): %s", program))
    end

    -- Check if program is explicitly whitelisted or wildcard "*" is granted
    if self._program_map["*"] == nil and self._program_map[program] == nil then
        error(string.format("Shell: program not in allowlist: %s", program))
    end
end

-- Check that each arg in args[] is permitted for the given program.
function Security:assert_args(program, args)
    args = args or {}
    local prog_base = tostring(program):match("[^/\\]+$") or tostring(program)

    -- 1. Always block known high-risk command execution switches
    for _, arg in ipairs(args) do
        local s = tostring(arg)
        if (prog_base == "git" and (s == "-c" or s:match("^%-c") or s == "--exec-path" or s:match("^%-%-exec%-path") or s == "--upload-pack")) or
           (prog_base == "find" and (s == "-exec" or s == "-execdir" or s == "-ok")) or
           (prog_base == "xargs" and (s:match("^%-I") or s:match("^%-%-replace"))) or
           (prog_base == "ssh" and (s:match("[pP]roxy[cC]ommand"))) then
            error(string.format("Shell: blocked dangerous argument for '%s': %s", program, s))
        end
    end

    -- 2. Whitelist check
    if self._program_map["*"] ~= nil and self._program_map["*"] == false then
        return -- wildcard any program and any args
    end

    local arg_whitelist = self._program_map[program]
    if arg_whitelist == false or arg_whitelist == nil then
        -- Allowed with general args (safe from known dangerous flags above)
        return
    end

    if type(arg_whitelist) == "table" then
        local whiteset = {}
        for _, a in ipairs(arg_whitelist) do whiteset[tostring(a)] = true end
        for _, arg in ipairs(args) do
            if not whiteset[tostring(arg)] then
                error(string.format("Shell: argument not in allowlist for '%s': %s", program, tostring(arg)))
            end
        end
    end
end

local function canonicalize_path(path)
    local parts = {}
    path = path:gsub("\\", "/")
    local is_absolute = path:sub(1, 1) == "/"
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
    if is_absolute then
        res = "/" .. res
    end
    return res
end

function Security:assert_scope(target_path, base_paths)
    if #self.scopes == 0 then return end
    local normalized = canonicalize_path(target_path)
    for _, pattern in ipairs(self.scopes) do
        local resolved = pattern:gsub("%$APPDATA", base_paths.appData)
                               :gsub("%$HOME", base_paths.home)
                               :gsub("%$DESKTOP", base_paths.desktop)
                               :gsub("\\", "/")
        local canonical_pattern = canonicalize_path(resolved)
        local escaped = canonical_pattern:gsub("([%(%)%.%%%+%-%?%[%^%$])", "%%%1")
        local lua_pattern = "^" .. escaped:gsub("%%%*", ".*") .. "$"
        if normalized:match(lua_pattern) then
            return true
        end
    end
    error(string.format("Accesso negato dallo Scope: %s", target_path))
end

local State = {}
State.__index = State

function State.new(json_codec)
    local self = setmetatable({}, State)
    self.json = json_codec
    return self
end

function State:update(key, callback)
    if type(callback) ~= "function" then
        error("State:update requires a function callback")
    end
    clib.platform_state_lock()
    local ok, res = pcall(function()
        local current = self:get(key)
        local next_val = callback(current)
        if next_val ~= nil then
            self:set(key, next_val)
        else
            self:delete(key)
        end
        return next_val
    end)
    clib.platform_state_unlock()
    if not ok then
        error(res)
    end
    return res
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
    local skey = tostring(key)
    if clib.platform_state_has(skey) == 0 then
        return nil
    end

    local num_buf = ffi.new("double[1]")
    if clib.platform_state_get_number(skey, num_buf) == 1 then
        return tonumber(num_buf[0])
    end

    local str_buf = ffi.new("char[65536]")
    if clib.platform_state_get_string(skey, str_buf, 65536) == 1 then
        local s = ffi.string(str_buf)
        if s == "true" then return true end
        if s == "false" then return false end
        return s
    end

    if clib.platform_state_get_json(skey, str_buf, 65536) == 1 then
        return self.json.decode(ffi.string(str_buf))
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
    local f = io.open(self.path, "rb")
    if f then
        local raw = f:read("*a")
        f:close()
        self.data = self.json.decode(raw) or {}
    end
end

function Store:save()
    local parent = self.path:match("^(.*)/[^/]+$")
    if parent and parent ~= "" then
        safe_mkdir_p(parent)
    end
    local f = io.open(self.path, "wb")
    if f then
        f:write(self.json.encode(self.data))
        f:close()
        return true
    end
    return false
end

function Store:get(key)
    return self.data[tostring(key)]
end

function Store:set(key, val)
    self.data[tostring(key)] = val
    return self:save()
end

function Store:has(key)
    return self.data[tostring(key)] ~= nil
end

function Store:delete(key)
    self.data[tostring(key)] = nil
    return self:save()
end

function Store:clear()
    self.data = {}
    return self:save()
end

function Store:entries()
    return self.data
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
          setFocus: function() { return makeInvoke("tauri:window:set_focus", { label: label }); },
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
      message: function(m, o) { o = o || {}; return makeInvoke("tauri:dialog:message", { message: m, title: o.title || "Message" }); },
      ask: function(m, o) { o = o || {}; return makeInvoke("tauri:dialog:ask", { message: m, title: o.title || "Confirm" }); }
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

local Tauri = {}
Tauri.__index = Tauri

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
    local app_name = (config.package and config.package.name) or config.productName or "tauri-lua-app"
    if ffi.os == "Linux" then
        local linux_paths = {
            "/usr/share/" .. app_name .. "/rocks/share/lua/5.1/?.lua",
            "/usr/share/" .. app_name .. "/rocks/share/lua/5.1/?/init.lua",
            "/usr/local/share/" .. app_name .. "/rocks/share/lua/5.1/?.lua",
            "/usr/local/share/" .. app_name .. "/rocks/share/lua/5.1/?/init.lua"
        }
        local linux_cpaths = {
            "/usr/share/" .. app_name .. "/rocks/lib/lua/5.1/?.so",
            "/usr/local/share/" .. app_name .. "/rocks/lib/lua/5.1/?.so"
        }
        for _, p in ipairs(linux_paths) do
            if not package.path:find(p, 1, true) then
                package.path = package.path .. ";" .. p
            end
        end
        for _, cp in ipairs(linux_cpaths) do
            if not package.cpath:find(cp, 1, true) then
                package.cpath = package.cpath .. ";" .. cp
            end
        end
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

    local sec_conf = (config.tauri and config.tauri.security) or config.security or {}
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
    local home = os.getenv("HOME") or os.getenv("USERPROFILE") or "."
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
    pcall(function() clib.platform_set_assets_path(dist_dir) end)

    local dev_url = build_conf.devUrl or build_conf.devPath
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

function Tauri:_register_builtins()
    self:register("tauri:state:get", function(payload)
        return self.state:get(payload.key)
    end)

    self:register("tauri:state:set", function(payload)
        self.state:set(payload.key, payload.value)
        return true
    end)

    self:register("tauri:store:get", function(payload)
        return self.store:get(payload.key)
    end)

    self:register("tauri:store:set", function(payload)
        return self.store:set(payload.key, payload.value)
    end)

    self:register("tauri:store:has", function(payload)
        return self.store:has(payload.key)
    end)

    self:register("tauri:store:delete", function(payload)
        return self.store:delete(payload.key)
    end)

    self:register("tauri:store:clear", function()
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

        while true do
            local n = clib.platform_process_read_line(handle, buf, buf_size)
            if n <= 0 then
                break
            end
            local line = ffi.string(buf, n)
            table.insert(lines, line)
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
        self:emit(payload.event, payload.payload)
        return true
    end)

    self:register("tauri:window:create", function(payload)
        self.security:assert_capability("core:default")
        local label = payload.label or ("win_" .. tostring(os.time()))
        local title = payload.title or "New Window"
        local url = payload.url or self.default_origin
        local width = payload.width or 800
        local height = payload.height or 600
        local resizable = payload.resizable ~= false and 1 or 0
        local decorations = payload.decorations ~= false and 1 or 0
        local transparent = payload.transparent and 1 or 0

        local ok = clib.platform_window_create(label, title, url, width, height, resizable, decorations, transparent)
        if ok == 1 then
            clib.platform_window_eval(label, TAURI_CLIENT_SCRIPT)
            self.windows[label] = true
            return true
        end
        error("Impossibile creare la finestra: " .. label)
    end)

    self:register("tauri:window:close", function(payload, caller_label)
        local target = (payload and payload.label) or caller_label or "main"
        clib.platform_window_close(target)
        return true
    end)

    self:register("tauri:window:destroy", function(payload, caller_label)
        local target = (payload and payload.label) or caller_label or "main"
        clib.platform_window_force_close(target)
        self.windows[target] = nil
        return true
    end)

    self:register("tauri:window:force_close", function(payload, caller_label)
        local target = (payload and payload.label) or caller_label or "main"
        clib.platform_window_force_close(target)
        self.windows[target] = nil
        return true
    end)

    self:register("tauri:window:set_title", function(payload, caller_label)
        local target = payload.label or caller_label
        clib.platform_window_set_title(target, tostring(payload.title or ""))
        return true
    end)

    self:register("tauri:window:set_size", function(payload, caller_label)
        local target = payload.label or caller_label
        clib.platform_window_set_size(target, tonumber(payload.width) or 800, tonumber(payload.height) or 600)
        return true
    end)

    self:register("tauri:window:minimize", function(payload, caller_label)
        clib.platform_window_minimize(payload.label or caller_label)
        return true
    end)

    self:register("tauri:window:maximize", function(payload, caller_label)
        clib.platform_window_maximize(payload.label or caller_label)
        return true
    end)

    self:register("tauri:window:show", function(payload, caller_label)
        clib.platform_window_show(payload.label or caller_label)
        return true
    end)

    self:register("tauri:window:hide", function(payload, caller_label)
        clib.platform_window_hide(payload.label or caller_label)
        return true
    end)

    self:register("tauri:window:set_focus", function(payload, caller_label)
        clib.platform_window_set_focus(payload.label or caller_label)
        return true
    end)

    self:register("tauri:window:start_dragging", function(payload, caller_label)
        clib.platform_window_start_dragging(caller_label)
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
        clib.platform_shortcut_unregister(payload.shortcut or "")
        return true
    end)

    self:register("tauri:updater:check", function()
        return {
            shouldUpdate = true,
            version = "1.0.1",
            date = "2026-10-13",
            body = "Aggiornamento disponibile."
        }
    end)

    self:register("tauri:updater:install", function()
        local ok = clib.platform_updater_install("update.tar.gz")
        return ok == 1
    end)

    self:register("tauri:fs:read_text_file", function(payload)
        self.security:assert_capability("fs:read-text-file")
        self.security:assert_scope(payload.path, self.base_paths)
        local f, err = io.open(payload.path, "r")
        if not f then error(err) end
        local content = f:read("*a")
        f:close()
        return content
    end)

    self:register("tauri:fs:write_text_file", function(payload)
        self.security:assert_capability("fs:write-text-file")
        self.security:assert_scope(payload.path, self.base_paths)
        local f, err = io.open(payload.path, "w")
        if not f then error(err) end
        f:write(tostring(payload.contents or ""))
        f:close()
        return true
    end)

    self:register("tauri:fs:read_dir", function(payload)
        self.security:assert_capability("fs:read-dir")
        self.security:assert_scope(payload.path, self.base_paths)
        local entries = {}
        if ffi.os == "Windows" then
            local search_path = payload.path:gsub("[/\\]+$", "") .. "\\*"
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
            local dir = clib.opendir(payload.path)
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
        self.security:assert_capability("fs:write-text-file")
        self.security:assert_scope(payload.path, self.base_paths)
        if clib.remove(payload.path) ~= 0 then
            error("Impossibile eliminare: " .. payload.path)
        end
        return true
    end)

    self:register("tauri:dialog:open", function(payload)
        self.security:assert_capability("dialog:open")
        local buf = ffi.new("char[1024]")
        local ok = clib.platform_dialog_open(payload.title, nil, buf, 1024)
        return ok == 1 and ffi.string(buf) or nil
    end)

    self:register("tauri:dialog:save", function(payload)
        self.security:assert_capability("dialog:save")
        local buf = ffi.new("char[1024]")
        local ok = clib.platform_dialog_save(payload.title, nil, buf, 1024)
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
        return self:get_theme()
    end)

    self:register("tauri:notification:send", function(payload)
        self.security:assert_capability("notification:send")
        clib.platform_notification_send(payload.title or "App", payload.body or "")
        return true
    end)

    self:register("tauri:path:app_data_dir", function() return self.base_paths.appData end)
    self:register("tauri:path:home_dir", function() return self.base_paths.home end)
    self:register("tauri:path:desktop_dir", function() return self.base_paths.desktop end)

    self:register("tauri:menu:set", function(payload)
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
    local js = string.format(
        "if(window.__TAURI__&&window.__TAURI__.event){window.__TAURI__.event._dispatch(%s,%s);}",
        json.encode(event_name),
        json.encode(payload)
    )
    clib.platform_window_eval("*", js)
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

    clib.platform_window_create("main", title, url, width, height, resizable, decorations, transparent)
    clib.platform_window_eval("main", TAURI_CLIENT_SCRIPT)
    for _, p in ipairs(self.plugin_list or {}) do
        if type(p.js) == "string" and p.js ~= "" then
            clib.platform_window_eval("main", p.js)
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
        clib.platform_window_return_async(label, seq, 0, json.encode({ data = res }))
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

    return event._prevented and 1 or 0
end

function __TAURI_DISPATCH_SINGLE_INSTANCE__(payload_json)
    if not g_tauri_instance then return end
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
Tauri.Submenu = Submenu
Tauri.MenuItem = MenuItem
Tauri.CheckMenuItem = CheckMenuItem
Tauri.PredefinedMenuItem = PredefinedMenuItem

return {
    init = Tauri.new,
    sleep = Tauri.sleep,
    json = json,
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
    Menu = Menu,
    Submenu = Submenu,
    MenuItem = MenuItem,
    CheckMenuItem = CheckMenuItem,
    PredefinedMenuItem = PredefinedMenuItem
}