local ffi = require("ffi")

ffi.cdef[[
    int kill(int pid, int sig);
    int usleep(unsigned int usec);
]]

-- ==========================================================
-- Minimal Color & Print Helpers
-- ==========================================================
local colors = {
    reset   = "\27[0m",
    bold    = "\27[1m",
    dim     = "\27[2m",
    red     = "\27[31m",
    green   = "\27[32m",
    yellow  = "\27[33m",
    blue    = "\27[34m",
    magenta = "\27[35m",
    cyan    = "\27[36m",
    white   = "\27[37m"
}

local function log_tag(tag, tag_color, msg)
    io.stdout:write(string.format("%s%12s%s %s\n", tag_color .. colors.bold, tag, colors.reset, msg))
    io.stdout:flush()
end

local function info(msg)    log_tag("Info", colors.blue, msg) end
local function success(msg) log_tag("Success", colors.green, msg) end
local function warn(msg)    log_tag("Warning", colors.yellow, msg) end
local function err(msg)     log_tag("Error", colors.red, msg) end
local function running(msg) log_tag("Running", colors.cyan, colors.dim .. "`" .. msg .. "`" .. colors.reset) end

-- ==========================================================
-- Strict RFC 8259 JSON Parser (Encode & Decode)
-- ==========================================================
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

    skip_ws()
    local val, err = parse_value()
    if err then return nil, err end
    skip_ws()
    if pos <= len then
        return nil, "trailing garbage after top-level JSON value at position " .. pos
    end
    return val
end

-- ==========================================================
-- Utility Functions
-- ==========================================================
local function file_exists(path)
    local f = io.open(path, "r")
    if f then
        f:close()
        return true
    end
    return false
end

local function read_file(path)
    local f = io.open(path, "rb")
    if not f then return nil end
    local content = f:read("*a")
    f:close()
    return content
end

local function write_file(path, content)
    local f = io.open(path, "wb")
    if not f then return false end
    f:write(content)
    f:close()
    return true
end

local function exec(cmd)
    local ok = os.execute(cmd)
    if ok ~= 0 and ok ~= true then
        error("Command failed: " .. cmd)
    end
end

local function shell_quote(str)
    local s = tostring(str)
    if ffi.os == "Windows" then
        s = s:gsub("%%", "%%%%")
        s = s:gsub('(\\*)"', '%1%1\\"')
        local trailing_bs = s:match("(\\+)$")
        if trailing_bs then
            s = s .. trailing_bs
        end
        return '"' .. s .. '"'
    else
        return "'" .. s:gsub("'", "'\\''") .. "'"
    end
end

local function make_dir(path)
    if ffi.os == "Windows" then
        local win_path = path:gsub("/", "\\")
        os.execute('mkdir "' .. win_path .. '" 2>NUL')
    else
        exec("mkdir -p " .. shell_quote(path))
    end
end

local function remove_dir_recursive(path)
    if not file_exists(path) then return end
    if ffi.os == "Windows" then
        local win_path = path:gsub("/", "\\")
        os.execute('rmdir /s /q "' .. win_path .. '" 2>NUL')
    else
        os.execute("rm -rf " .. shell_quote(path) .. " 2>/dev/null")
    end
end

local function copy_dir_recursive(src, dest)
    if ffi.os == "Windows" then
        local win_src = src:gsub("/", "\\")
        local win_dest = dest:gsub("/", "\\")
        make_dir(dest)
        os.execute('xcopy /E /I /Y /Q "' .. win_src .. '" "' .. win_dest .. '\\" >NUL 2>NUL')
    else
        exec("mkdir -p " .. shell_quote(dest))
        exec("cp -R " .. shell_quote(src) .. "/. " .. shell_quote(dest) .. "/")
    end
end

local function get_file_mtime(path)
    local p
    local str_path = tostring(path)
    if ffi.os == "OSX" then
        p = io.popen("stat -f '%m' " .. shell_quote(str_path) .. " 2>/dev/null")
    elseif ffi.os == "Linux" then
        p = io.popen("stat -c '%Y' " .. shell_quote(str_path) .. " 2>/dev/null")
    else
        local win_path = str_path:gsub("/", "\\"):gsub("'", "''")
        p = io.popen('powershell -NoProfile -Command "(Get-Item -LiteralPath \'' .. win_path .. '\').LastWriteTimeUtc.Ticks" 2>NUL')
    end
    if not p then return 0 end
    local res = p:read("*a")
    p:close()
    return tonumber(res) or 0
end

local function find_watched_files()
    local files = {}
    if ffi.os == "Windows" then
        local p = io.popen('powershell -NoProfile -Command "Get-ChildItem -Path src-tauri,src,plugins -Include *.lua,tauri.conf.json -Recurse -File -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName" 2>NUL')
        if p then
            for line in p:lines() do
                if line ~= "" then table.insert(files, line) end
            end
            p:close()
        end
    else
        local p = io.popen("find src-tauri/src src/ plugins/ ../lua-tauri-plugins/plugins/ tauri.conf.json src-tauri/tauri.conf.json -name '*.lua' -o -name 'tauri.conf.json' 2>/dev/null")
        if p then
            for line in p:lines() do
                if line ~= "" then
                    table.insert(files, line)
                end
            end
            p:close()
        end
    end
    return files
end

-- ==========================================================
-- Config Resolution
-- ==========================================================
local function load_config()
    local conf_path = nil
    if file_exists("src-tauri/tauri.conf.json") then
        conf_path = "src-tauri/tauri.conf.json"
    elseif file_exists("tauri.conf.json") then
        conf_path = "tauri.conf.json"
    else
        error("File tauri.conf.json non trovato in root né in src-tauri/")
    end

    local raw = read_file(conf_path)
    local parsed = json.decode(raw) or {}
    return parsed, conf_path
end

local function get_app_meta(conf)
    local name = (conf.package and conf.package.name) or conf.productName or "tauri-lua-app"
    local version = (conf.package and conf.package.version) or conf.version or "0.1.0"
    local identifier = conf.identifier or "com.tauri.lua"
    return name, version, identifier
end

local ENGINE_ROOT = os.getenv("TAURI_LUA_ROOT")
if not ENGINE_ROOT or ENGINE_ROOT == "" then
    local script_dir = arg and arg[0] and arg[0]:match("^(.*)/[^/]+$")
    if script_dir then
        ENGINE_ROOT = script_dir .. "/.."
    else
        ENGINE_ROOT = "."
    end
end

-- ==========================================================
-- Ensure Runtime Binary is Built
-- ==========================================================
local function ensure_runtime_built(release_mode)
    local bin = "build/tauri_app"
    if ffi.os == "Windows" then
        bin = file_exists("build/Release/tauri_app.exe") and "build/Release/tauri_app.exe" or "build/tauri_app.exe"
    end

    if not release_mode then
        if file_exists(bin) then return bin end
        if file_exists(ENGINE_ROOT .. "/build/tauri_app") then
            return ENGINE_ROOT .. "/build/tauri_app"
        end
    end

    if release_mode or not file_exists(bin) then
        local build_type = release_mode and "Release" or "Debug"
        info("Compiling Tauri-Lua runtime (" .. build_type .. ")...")
        local p = io.popen("pwd 2>/dev/null")
        local cwd = p and p:read("*l") or "."
        if p then p:close() end

        -- Force clean regeneration of embedded headers in release mode to avoid stale dev/fallback artifacts
        if release_mode then
            if file_exists("build/generated") then
                remove_dir_recursive("build/generated")
            end
        end

        exec("cmake -B build -S " .. shell_quote(ENGINE_ROOT) .. " -DAPP_DIR=" .. shell_quote(cwd) .. " -DCMAKE_BUILD_TYPE=" .. shell_quote(build_type) .. " > /dev/null")
        exec("cmake --build build --config " .. shell_quote(build_type) .. " > /dev/null")
        success("Runtime built successfully.")
        if ffi.os == "Windows" then
            bin = file_exists("build/Release/tauri_app.exe") and "build/Release/tauri_app.exe" or "build/tauri_app.exe"
        else
            bin = "build/tauri_app"
        end
    end
    return bin
end

-- ==========================================================
-- Hermetic LuaRocks Dependency Management & Bundler
-- ==========================================================

local function parse_rock_server_url(srv)
    if type(srv) ~= "string" or srv == "" then
        return nil, "Rock server URL must be a non-empty string"
    end
    if srv:find("[;&|`$><()\\!%c%s]") then
        return nil, "Security error: invalid characters in rock server URL"
    end
    local scheme, rest = srv:match("^([a-zA-Z][a-zA-Z0-9%+%-%.]*)://(.*)$")
    if not scheme then
        return nil, "Security error: invalid rock server URL (missing scheme)"
    end
    scheme = scheme:lower()
    if scheme ~= "https" then
        return nil, "Security error: rock server must use HTTPS (received: '" .. scheme .. "')"
    end
    local host_port, path = rest:match("^([^/]+)(/?.*)$")
    if not host_port or host_port == "" then
        return nil, "Security error: missing host in rock server URL"
    end
    local host, port = host_port:match("^([^:]+):?(%d*)$")
    if not host or host == "" then
        return nil, "Security error: invalid host in rock server URL"
    end
    if not host:match("^[a-zA-Z0-9%-_%.]+$") then
        return nil, "Security error: invalid hostname in rock server URL ('" .. host .. "')"
    end
    return {
        scheme = scheme,
        host = host,
        port = (port ~= "" and tonumber(port)) or 443,
        path = (path ~= "" and path) or "/"
    }, nil
end

local function validate_rock_server(srv, allow_custom_server)
    if not srv or srv == "" then return nil, nil end
    local parsed, err_msg = parse_rock_server_url(srv)
    if not parsed then return nil, err_msg end

    local is_default = (srv == "https://luarocks.org" or srv == "https://luarocks.org/" or srv:match("^https://luarocks%.org/"))
    if not is_default and not allow_custom_server then
        return nil, "Security error: custom rock server '" .. srv .. "' requires explicit opt-in (--allow-custom-server flag or allow_custom_rock_server: true in tauri.conf.json)"
    end
    return srv, nil
end

local function validate_rock_name(name)
    if type(name) ~= "string" or name == "" then
        return nil, "rock name must be a non-empty string"
    end
    if name:find("[;&|`$><()\\!%c%s]") then
        return nil, "Security error: invalid shell characters in rock name"
    end
    if not name:match("^[a-zA-Z0-9%-_%.]+$") then
        return nil, "Security error: invalid format for rock name '" .. name .. "' (only alphanumeric, '-', '_', '.' allowed)"
    end
    return name, nil
end

local function is_pinned_version(ver)
    if type(ver) ~= "string" or ver == "" then return false end
    if ver:find("[%s%<%>%~%*%^]") then return false end
    return ver:match("^[0-9]+%.[0-9]+.*$") ~= nil or ver:match("^[0-9]+%-[0-9]+$") ~= nil
end

local function validate_rock_version(ver, require_pinned)
    if not ver or ver == "" then
        if require_pinned then
            return nil, "Security error: unpinned dependency detected in build mode. Exact pinned version (e.g. '1.2.3-1') is required for build reproducibility."
        end
        return nil, nil
    end
    if type(ver) ~= "string" then
        return nil, "rock version must be a string"
    end
    if ver:find("[;&|`$><()\\!%c]") then
        return nil, "Security error: invalid characters in rock version ('" .. ver .. "')"
    end
    if require_pinned and not is_pinned_version(ver) then
        return nil, "Security error: rock version '" .. ver .. "' is not pinned. Build mode requires exact pinned versions (e.g. '1.2.3-1')."
    end
    if not ver:match("^[a-zA-Z0-9%-_%.%~%s%<%>%=>%:]+$") then
        return nil, "Security error: invalid format for rock version '" .. ver .. "'"
    end
    return ver, nil
end

local function get_rocks_tree_dir()
    if file_exists("src-tauri") then
        return "src-tauri/.rocks"
    end
    return ".rocks"
end

local function get_lockfile_path()
    if file_exists("src-tauri") then
        return "src-tauri/tauri.lock"
    end
    return "tauri.lock"
end

local function compute_file_checksum(filepath)
    if not file_exists(filepath) then return "unknown" end
    local cmd
    if ffi.os == "OSX" then
        cmd = "shasum -a 256 " .. shell_quote(filepath) .. " 2>/dev/null"
    elseif ffi.os == "Linux" then
        cmd = "sha256sum " .. shell_quote(filepath) .. " 2>/dev/null"
    else
        local ps_path = filepath:gsub("/", "\\"):gsub("'", "''")
        cmd = 'powershell -NoProfile -Command "(Get-FileHash -LiteralPath \'' .. ps_path .. '\' -Algorithm SHA256).Hash.ToLower()" 2>NUL'
    end
    local p = io.popen(cmd)
    if p then
        local out = p:read("*l")
        p:close()
        if out then
            local hash = out:match("(%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x)")
            if hash then return "sha256:" .. hash end
        end
    end
    local content = read_file(filepath)
    if content then
        local h = 5381
        for i = 1, #content do
            h = (h * 33 + string.byte(content, i)) % 4294967296
        end
        return string.format("hash:%08x", h)
    end
    return "unknown"
end

local function get_installed_rock_files(rocks_dir, rock_name, ver)
    local base = rocks_dir .. "/lib/luarocks/rocks-5.1/" .. rock_name .. "/" .. ver
    local manifest_file = base .. "/rock_manifest"
    local files = {}
    if file_exists(manifest_file) then
        table.insert(files, manifest_file)
        local content = read_file(manifest_file)
        if content then
            local env = {}
            local fn = loadstring(content)
            if fn then
                setfenv(fn, env)
                pcall(fn)
                local mf = env.rock_manifest
                if type(mf) == "table" then
                    if type(mf.lua) == "table" then
                        for rel, _ in pairs(mf.lua) do
                            local p = rocks_dir .. "/share/lua/5.1/" .. rel
                            if file_exists(p) then table.insert(files, p) end
                        end
                    end
                    if type(mf.lib) == "table" then
                        for rel, _ in pairs(mf.lib) do
                            local p = rocks_dir .. "/lib/lua/5.1/" .. rel
                            if file_exists(p) then table.insert(files, p) end
                        end
                    end
                end
            end
        end
    end
    table.sort(files)
    return files
end

local function compute_composite_checksum(file_list)
    if not file_list or #file_list == 0 then return "sha256:unknown" end
    if #file_list == 1 then
        return compute_file_checksum(file_list[1])
    end
    local quoted = {}
    for _, f in ipairs(file_list) do
        table.insert(quoted, shell_quote(f))
    end
    local file_args = table.concat(quoted, " ")
    local cmd
    if ffi.os == "OSX" then
        cmd = "shasum -a 256 " .. file_args .. " 2>/dev/null | shasum -a 256 2>/dev/null"
    elseif ffi.os == "Linux" then
        cmd = "sha256sum " .. file_args .. " 2>/dev/null | sha256sum 2>/dev/null"
    else
        local hashes = {}
        for _, f in ipairs(file_list) do
            table.insert(hashes, compute_file_checksum(f))
        end
        local combined = table.concat(hashes, "\n")
        local h = 5381
        for i = 1, #combined do
            h = (h * 33 + string.byte(combined, i)) % 4294967296
        end
        return string.format("hash:%08x", h)
    end
    local p = io.popen(cmd)
    if p then
        local out = p:read("*l")
        p:close()
        if out then
            local hash = out:match("(%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x)")
            if hash then return "sha256:" .. hash end
        end
    end
    return compute_file_checksum(file_list[1])
end

local function get_installed_rock_details(rocks_dir, rock_name)
    local base = rocks_dir .. "/lib/luarocks/rocks-5.1/" .. rock_name
    if not file_exists(base) then return nil end
    local p
    if ffi.os == "Windows" then
        p = io.popen('dir /b /ad "' .. base:gsub("/", "\\") .. '" 2>NUL')
    else
        p = io.popen("ls -1 " .. shell_quote(base) .. " 2>/dev/null")
    end
    if not p then return nil end
    local versions = {}
    for line in p:lines() do
        local trimmed = line:match("^%s*(.-)%s*$")
        if trimmed ~= "" and trimmed ~= "." and trimmed ~= ".." then
            table.insert(versions, trimmed)
        end
    end
    p:close()
    if #versions == 0 then return nil end
    local ver = versions[1]
    local installed_files = get_installed_rock_files(rocks_dir, rock_name, ver)
    local checksum = "sha256:unknown"
    if #installed_files > 0 then
        checksum = compute_composite_checksum(installed_files)
    end
    return {
        version = ver,
        checksum = checksum,
        files = installed_files
    }
end

local function read_lockfile()
    local lock_path = get_lockfile_path()
    if not file_exists(lock_path) then return nil end
    local content = read_file(lock_path)
    if not content or content == "" then return nil end
    return json.decode(content)
end

local function save_lockfile(lock_data)
    local lock_path = get_lockfile_path()
    local lines = {}
    table.insert(lines, "{")
    table.insert(lines, '  "version": 1,')
    table.insert(lines, '  "rocks": {')
    local keys = {}
    for k in pairs(lock_data.rocks or {}) do table.insert(keys, k) end
    table.sort(keys)
    for i, k in ipairs(keys) do
        local r = lock_data.rocks[k]
        local comma = (i == #keys) and "" or ","
        table.insert(lines, string.format('    "%s": {', k))
        table.insert(lines, string.format('      "version": "%s",', r.version or ""))
        table.insert(lines, string.format('      "checksum": "%s",', r.checksum or ""))
        table.insert(lines, string.format('      "server": "%s"', r.server or "https://luarocks.org"))
        table.insert(lines, string.format('    }%s', comma))
    end
    table.insert(lines, "  }")
    table.insert(lines, "}")
    write_file(lock_path, table.concat(lines, "\n") .. "\n")
end

local function find_rockspec_file()
    local search_dirs = { "src-tauri", "." }
    for _, dir in ipairs(search_dirs) do
        local probe = (ffi.os == "Windows")
            and ('dir /b "' .. dir:gsub("/", "\\") .. '\\*.rockspec" 2>NUL')
            or ("ls " .. dir .. "/*.rockspec 2>/dev/null")
        local p = io.popen(probe)
        if p then
            local file = p:read("*l")
            p:close()
            if file and file ~= "" then
                local full = (dir == "." and file or (dir .. "/" .. file))
                if file_exists(full) then
                    return full
                end
            end
        end
    end
    return nil
end

local function find_luarocks_bin()
    local probe = (ffi.os == "Windows") and "where luarocks 2>NUL" or "which luarocks 2>/dev/null"
    local p = io.popen(probe)
    local out = p and p:read("*l")
    if p then p:close() end
    if out and out ~= "" and file_exists(out) then
        return out
    end
    local candidates = {
        "/opt/homebrew/bin/luarocks",
        "/usr/local/bin/luarocks",
        "/usr/bin/luarocks",
        "/snap/bin/luarocks",
        "C:\\Program Files\\LuaRocks\\luarocks.bat",
        "C:\\Program Files\\LuaRocks\\luarocks.exe",
        "C:\\Program Files (x86)\\LuaRocks\\luarocks.bat",
        "C:\\ProgramData\\chocolatey\\bin\\luarocks.bat",
        "C:\\ProgramData\\chocolatey\\bin\\luarocks.exe"
    }
    if ffi.os == "Windows" then
        local appdata = os.getenv("APPDATA")
        if appdata then
            table.insert(candidates, appdata .. "\\luarocks\\luarocks.bat")
        end
        local localappdata = os.getenv("LOCALAPPDATA")
        if localappdata then
            table.insert(candidates, localappdata .. "\\Programs\\LuaRocks\\luarocks.bat")
        end
        local userprofile = os.getenv("USERPROFILE")
        if userprofile then
            table.insert(candidates, userprofile .. "\\scoop\\apps\\luarocks\\current\\luarocks.bat")
        end
    end
    for _, c in ipairs(candidates) do
        if file_exists(c) then return c end
    end
    return nil
end

local function print_luarocks_missing_guidance()
    warn("LuaRocks CLI (`luarocks`) was not detected on your system.")
    io.stdout:write(colors.cyan .. "  -> To install LuaRocks for your operating system:\n" .. colors.reset)
    if ffi.os == "OSX" then
        io.stdout:write("     macOS:            brew install luarocks\n")
    elseif ffi.os == "Linux" then
        io.stdout:write("     Ubuntu / Debian:  sudo apt install luarocks\n")
        io.stdout:write("     Fedora / RHEL:    sudo dnf install luarocks\n")
        io.stdout:write("     Arch Linux:       sudo pacman -S luarocks\n")
    else
        io.stdout:write("     Windows:          winget install luarocks  (or: choco install luarocks)\n")
    end
    io.stdout:write("\n")
end

local function extract_luarocks_dependencies(conf)
    local list = {}
    local raw = nil
    if conf.dependencies and conf.dependencies.luarocks then
        raw = conf.dependencies.luarocks
    elseif conf.dependencies and conf.dependencies.rocks then
        raw = conf.dependencies.rocks
    elseif conf.tauri and conf.tauri.dependencies and conf.tauri.dependencies.luarocks then
        raw = conf.tauri.dependencies.luarocks
    elseif conf.rocks then
        raw = conf.rocks
    elseif conf.bundle and conf.bundle.luarocks then
        raw = conf.bundle.luarocks
    end

    if type(raw) == "table" then
        for _, item in ipairs(raw) do
            if type(item) == "string" then
                local name, ver = item:match("^([%w%-_%.]+)%s*(.*)$")
                if name then
                    table.insert(list, { name = name, version = (ver ~= "" and ver or nil) })
                end
            elseif type(item) == "table" and item.name then
                table.insert(list, {
                    name = item.name,
                    version = item.version,
                    server = item.server
                })
            end
        end
    end
    return list
end

local function is_rock_installed(rocks_dir, rock_name)
    local manifest_dir = rocks_dir .. "/lib/luarocks/rocks-5.1/" .. rock_name
    return file_exists(manifest_dir)
end

local function has_flag(args, flag)
    if not args then return false end
    for _, a in ipairs(args) do
        if a == flag then return true end
    end
    return false
end

local function install_single_rock(rock_def, rocks_dir, luarocks_bin, allow_custom)
    local name, name_err = validate_rock_name(rock_def.name)
    if not name then
        err(name_err)
        return false
    end

    local ver, ver_err = validate_rock_version(rock_def.version, false)
    if ver_err then
        err(ver_err)
        return false
    end

    local srv, srv_err = validate_rock_server(rock_def.server, allow_custom)
    if srv_err then
        err(srv_err)
        return false
    end

    local desc = name .. (ver and (" " .. ver) or "")
    info("Installing LuaRock `" .. desc .. "` into hermetic tree: " .. rocks_dir)

    local cmd = shell_quote(luarocks_bin) .. " --tree " .. shell_quote(rocks_dir) .. " --lua-version 5.1 install " .. shell_quote(name)
    if ver and ver ~= "" then
        cmd = cmd .. " " .. shell_quote(ver)
    end
    if srv and srv ~= "" then
        cmd = cmd .. " --server " .. shell_quote(srv)
    end

    running(cmd)
    local ok = os.execute(cmd)
    if ok == 0 or ok == true then
        success("Successfully installed `" .. desc .. "`")
        return true
    else
        err("Failed to install LuaRock `" .. desc .. "`")
        return false
    end
end

local function ensure_rocks_installed(conf, force, is_build, allow_custom_server)
    local rocks = extract_luarocks_dependencies(conf)
    local rockspec_file = find_rockspec_file()
    if #rocks == 0 and not rockspec_file then
        return true
    end

    local allow_custom = allow_custom_server or conf.allow_custom_rock_server or (conf.security and conf.security.allow_custom_rock_server) or false

    -- 1. Validate all rocks
    for _, rock_def in ipairs(rocks) do
        local name, name_err = validate_rock_name(rock_def.name)
        if not name then error(name_err) end
        local ver, ver_err = validate_rock_version(rock_def.version, is_build)
        if ver_err then error(ver_err) end
        local srv, srv_err = validate_rock_server(rock_def.server, allow_custom)
        if srv_err then error(srv_err) end
    end

    local rocks_dir = get_rocks_tree_dir()
    local luarocks_bin = find_luarocks_bin()
    local lock = read_lockfile()

    -- In build mode, verify against lockfile if present
    if is_build and lock and lock.rocks then
        for _, rock_def in ipairs(rocks) do
            local lock_entry = lock.rocks[rock_def.name]
            if not lock_entry then
                error("Lockfile missing entry for rock '" .. rock_def.name .. "'. Run 'lua-tauri dev' or 'lua-tauri rocks install' to generate tauri.lock first.")
            end
            if rock_def.version and lock_entry.version and rock_def.version ~= lock_entry.version then
                error("Lockfile mismatch for '" .. rock_def.name .. "': tauri.conf.json requires " .. rock_def.version .. " but tauri.lock specifies " .. lock_entry.version)
            end
            local details = get_installed_rock_details(rocks_dir, rock_def.name)
            if not details then
                error("LuaRock '" .. rock_def.name .. "' is not installed in hermetic tree '" .. rocks_dir .. "'")
            end
            if lock_entry.checksum and lock_entry.checksum ~= "" and lock_entry.checksum ~= "sha256:unknown" then
                if details.checksum ~= lock_entry.checksum then
                    error("Checksum verification failed for LuaRock '" .. rock_def.name .. "': lockfile expected " .. lock_entry.checksum .. " but found " .. details.checksum)
                end
            end
        end
    end

    if not luarocks_bin then
        local all_present = true
        for _, r in ipairs(rocks) do
            if not is_rock_installed(rocks_dir, r.name) then
                all_present = false
                break
            end
        end
        if all_present and not rockspec_file then
            return true
        end

        print_luarocks_missing_guidance()
        error("Cannot synchronize LuaRocks dependencies because `luarocks` executable was not found.")
    end

    if rockspec_file then
        info("Detected rockspec: `" .. rockspec_file .. "` - checking dependencies...")
        local cmd = shell_quote(luarocks_bin) .. " --tree " .. shell_quote(rocks_dir) .. " --lua-version 5.1 install --only-deps " .. shell_quote(rockspec_file)
        running(cmd)
        os.execute(cmd)
    end

    if #rocks > 0 then
        info("Checking hermetic LuaRocks tree `" .. rocks_dir .. "` (" .. #rocks .. " declared dependencies)...")
        local installed_count = 0
        for _, rock_def in ipairs(rocks) do
            if force or not is_rock_installed(rocks_dir, rock_def.name) then
                local ok = install_single_rock(rock_def, rocks_dir, luarocks_bin, allow_custom)
                if not ok then
                    error("LuaRocks installation aborted due to dependency error.")
                end
                installed_count = installed_count + 1
            end
        end

        if installed_count > 0 then
            success("Hermetic LuaRocks tree synchronized (" .. installed_count .. " packages installed/updated).")
        end

        -- Update or create lockfile only during dev/install, NEVER overwrite during production build
        if not is_build then
            local lock_data = lock or { version = 1, rocks = {} }
            lock_data.rocks = lock_data.rocks or {}
            for _, rock_def in ipairs(rocks) do
                local details = get_installed_rock_details(rocks_dir, rock_def.name)
                local installed_ver = (details and details.version) or rock_def.version or "unknown"
                local checksum = (details and details.checksum) or "sha256:unknown"
                lock_data.rocks[rock_def.name] = {
                    version = installed_ver,
                    checksum = checksum,
                    server = rock_def.server or "https://luarocks.org"
                }
            end
            save_lockfile(lock_data)
            info("Saved dependency lockfile: " .. get_lockfile_path())
        end
    end
    return true
end

local function cmd_rocks(args)
    local conf, conf_path = load_config()
    local rocks_dir = get_rocks_tree_dir()
    local sub = args[1] or "install"
    local allow_custom = has_flag(args, "--allow-custom-server")

    if sub == "install" then
        local rock_name = args[2]
        local rock_ver = args[3]
        local luarocks_bin = find_luarocks_bin()
        if not luarocks_bin then
            print_luarocks_missing_guidance()
            error("LuaRocks CLI is required to install dependencies.")
        end

        if rock_name then
            local ok = install_single_rock({ name = rock_name, version = rock_ver }, rocks_dir, luarocks_bin, allow_custom)
            if not ok then os.exit(1) end
        else
            ensure_rocks_installed(conf, true, false, allow_custom)
        end

    elseif sub == "list" then
        info("Hermetic LuaRocks tree: " .. rocks_dir)
        local luarocks_bin = find_luarocks_bin()
        if luarocks_bin then
            local cmd = shell_quote(luarocks_bin) .. " --tree " .. shell_quote(rocks_dir) .. " --lua-version 5.1 list"
            os.execute(cmd)
        else
            local probe = (ffi.os == "Windows")
                and ('dir /b "' .. (rocks_dir .. "/lib/luarocks/rocks-5.1"):gsub("/", "\\") .. '" 2>NUL')
                or ("ls -1 " .. shell_quote(rocks_dir .. "/lib/luarocks/rocks-5.1") .. " 2>/dev/null")
            local p = io.popen(probe)
            if p then
                for line in p:lines() do
                    if line ~= "" and line ~= "manifest" then
                        print("  - " .. line)
                    end
                end
                p:close()
            end
        end

    elseif sub == "remove" or sub == "uninstall" then
        local rock_name = args[2]
        if not rock_name or rock_name == "" then
            error("Usage: lua-tauri rocks remove <rock_name>")
        end
        local luarocks_bin = find_luarocks_bin()
        if not luarocks_bin then
            print_luarocks_missing_guidance()
            error("LuaRocks CLI is required.")
        end
        local name, err_msg = validate_rock_name(rock_name)
        if not name then error(err_msg) end
        info("Removing LuaRock `" .. name .. "` from " .. rocks_dir .. " ...")
        local cmd = shell_quote(luarocks_bin) .. " --tree " .. shell_quote(rocks_dir) .. " --lua-version 5.1 remove " .. shell_quote(name)
        os.execute(cmd)
        success("Removed `" .. name .. "` from " .. rocks_dir)

    elseif sub == "clean" then
        info("Removing hermetic LuaRocks tree: " .. rocks_dir)
        remove_dir_recursive(rocks_dir)
        success("Hermetic LuaRocks tree cleaned.")

    elseif sub == "path" then
        local luarocks_bin = find_luarocks_bin()
        if luarocks_bin then
            local cmd = shell_quote(luarocks_bin) .. " --tree " .. shell_quote(rocks_dir) .. " --lua-version 5.1 path"
            os.execute(cmd)
        else
            if ffi.os == "Windows" then
                print("set LUA_PATH=" .. rocks_dir .. "/share/lua/5.1/?.lua;" .. rocks_dir .. "/share/lua/5.1/?/init.lua;%LUA_PATH%")
                print("set LUA_CPATH=" .. rocks_dir .. "/lib/lua/5.1/?.dll;%LUA_CPATH%")
            else
                print("export LUA_PATH='" .. rocks_dir .. "/share/lua/5.1/?.lua;" .. rocks_dir .. "/share/lua/5.1/?/init.lua;$LUA_PATH'")
                print("export LUA_CPATH='" .. rocks_dir .. "/lib/lua/5.1/?.so;$LUA_CPATH'")
            end
        end
    else
        err("Unknown rocks subcommand: " .. sub)
        print("Usage: lua-tauri rocks [install [name] [version] | list | remove <name> | clean | path]")
        os.exit(1)
    end
end

-- ==========================================================
-- Command: DEV
-- ==========================================================
local function cmd_dev(args)
    local conf, conf_path = load_config()
    local app_name, version = get_app_meta(conf)
    local build_conf = conf.build or {}

    info("Starting Tauri-Lua development for `" .. app_name .. "` v" .. version)

    -- Ensure hermetic LuaRocks dependencies are synchronized
    ensure_rocks_installed(conf, false, false, has_flag(args, "--allow-custom-server"))

    local dev_url = build_conf.devUrl or build_conf.devPath

    local dev_server_pid = nil
    local before_dev = build_conf.beforeDevCommand
    if before_dev and before_dev ~= "" then
        running(before_dev)
        local log_redirect = " > .dev_server.log 2>&1"
        local p
        if ffi.os == "Windows" then
            p = io.popen("start /B " .. before_dev .. log_redirect)
        else
            p = io.popen("(" .. before_dev .. ")" .. log_redirect .. " & echo $!")
        end
        if p then
            dev_server_pid = tonumber(p:read("*l"))
            p:close()
        end
    end

    if dev_url and type(dev_url) == "string" and dev_url ~= "" then
        if not dev_url:match("^https?://[a-zA-Z0-9%.%-_]+:%d+/?$") and not dev_url:match("^https?://[a-zA-Z0-9%.%-_]+/?$") then
            error("Invalid or insecure devUrl format in configuration: " .. tostring(dev_url))
        end
        info("Waiting for dev server at " .. dev_url .. " ...")
        local ready = false
        for i = 1, 60 do
            local p = io.popen("curl -s -o /dev/null -w '%{http_code}' " .. shell_quote(dev_url) .. " 2>/dev/null")
            local code = p and p:read("*a")
            if p then p:close() end
            if code and tonumber(code) and tonumber(code) > 0 then
                ready = true
                break
            end
            ffi.C.usleep(500000) -- 500ms
        end
        if ready then
            success("Dev server is ready!")
        else
            warn("Dev server did not answer within timeout, proceeding anyway...")
        end
    end

    local bin_path = ensure_runtime_built(false)

    if ffi.os == "OSX" then
        local app_bundle_dir = "build/" .. app_name .. ".app"
        local macos_dir = app_bundle_dir .. "/Contents/MacOS"
        local frameworks_dir = app_bundle_dir .. "/Contents/Frameworks"
        exec("mkdir -p " .. shell_quote(macos_dir) .. " " .. shell_quote(frameworks_dir))
        
        local app_bin = macos_dir .. "/" .. app_name
        exec("cp " .. shell_quote(bin_path) .. " " .. shell_quote(app_bin))
        
        local dylib_source = ENGINE_ROOT .. "/build/libtaurilua.dylib"
        if not file_exists(dylib_source) then dylib_source = "build/libtaurilua.dylib" end
        if file_exists(dylib_source) then
            exec("ln -sf " .. shell_quote(dylib_source) .. " " .. shell_quote(frameworks_dir .. "/libtaurilua.dylib"))
        end
        
        local plist = string.format([[<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>%s</string>
    <key>CFBundleIdentifier</key>
    <string>%s</string>
    <key>CFBundleName</key>
    <string>%s</string>
    <key>CFBundleDisplayName</key>
    <string>%s</string>
    <key>CFBundleVersion</key>
    <string>%s</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleIconFile</key>
    <string>icon</string>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>]], app_name, identifier, app_name, app_name, version)
        if file_exists("src-tauri/icons/icon.icns") then
            exec("mkdir -p " .. shell_quote(app_bundle_dir .. "/Contents/Resources"))
            exec("cp src-tauri/icons/icon.icns " .. shell_quote(app_bundle_dir .. "/Contents/Resources/icon.icns"))
        end
        write_file(app_bundle_dir .. "/Contents/Info.plist", plist)
        exec("codesign --force --deep --sign - " .. shell_quote(app_bundle_dir) .. " 2>/dev/null || true")
        bin_path = app_bin
    end

    -- Watch timestamps table
    local file_timestamps = {}
    for _, f in ipairs(find_watched_files()) do
        file_timestamps[f] = get_file_mtime(f)
    end

    local function spawn_app()
        running(bin_path)
        -- TAURILUA_DEV=1 activates cwd-relative module discovery (./plugins, ../lua-tauri-plugins, etc.)
        -- which is deliberately excluded from release builds for security.
        local p
        if ffi.os == "Windows" then
            p = io.popen("set TAURILUA_DEV=1 && start /B " .. shell_quote(bin_path))
        else
            p = io.popen("TAURILUA_DEV=1 " .. shell_quote(bin_path) .. " > /dev/null 2>&1 & echo $!")
        end
        local pid = p and tonumber(p:read("*l"))
        if p then p:close() end
        return pid
    end

    local app_pid = spawn_app()
    info("Watching for backend and config changes (Hot Reload enabled)...")

    local function cleanup()
        io.stdout:write("\n")
        info("Shutting down development processes...")
        if app_pid and ffi.C.kill(app_pid, 0) == 0 then
            ffi.C.kill(app_pid, 9)
        end
        if dev_server_pid and ffi.C.kill(dev_server_pid, 0) == 0 then
            ffi.C.kill(dev_server_pid, 9)
        end
        pcall(function() os.remove(".dev_server.log") end)
        os.exit(0)
    end

    -- Run watch loop
    while true do
        ffi.C.usleep(300000) -- poll every 300ms

        -- Check if child application process is still alive
        if app_pid and ffi.C.kill(app_pid, 0) ~= 0 then
            info("Application closed, stopping dev session.")
            cleanup()
        end

        local changed_file = nil
        local current_files = find_watched_files()
        for _, f in ipairs(current_files) do
            local mtime = get_file_mtime(f)
            if file_timestamps[f] and mtime > file_timestamps[f] then
                changed_file = f
                file_timestamps[f] = mtime
                break
            elseif not file_timestamps[f] then
                changed_file = f
                file_timestamps[f] = mtime
                break
            end
        end

        if changed_file then
            log_tag("Reload", colors.magenta, "Detected change in " .. changed_file .. ", reloading backend...")
            if app_pid then
                ffi.C.kill(app_pid, 9)
            end
            ffi.C.usleep(50000) -- 50ms pause
            app_pid = spawn_app()
        end
    end
end

-- ==========================================================
-- Command: BUILD
-- ==========================================================
local function cmd_build(args)
    local conf, conf_path = load_config()
    local app_name, version, identifier = get_app_meta(conf)
    local build_conf = conf.build or {}
    local bundle_conf = conf.bundle or (conf.tauri and conf.tauri.bundle) or {}

    info("Building application `" .. app_name .. "` v" .. version .. " (" .. identifier .. ")")

    -- Ensure hermetic LuaRocks dependencies are synchronized before bundle packaging
    ensure_rocks_installed(conf, false, true, has_flag(args, "--allow-custom-server"))

    local before_build = build_conf.beforeBuildCommand
    if before_build and before_build ~= "" then
        running(before_build)
        exec(before_build)
    end

    local frontend_dist = build_conf.frontendDist or build_conf.distDir or bundle_conf.distDir or "dist"
    if frontend_dist:sub(1, 3) == "../" then
        frontend_dist = frontend_dist:sub(4)
    end

    if not file_exists(frontend_dist) then
        error("frontendDist non trovata: " .. frontend_dist)
    end

    -- Build runtime in release mode
    local bin_path = ensure_runtime_built(true)

    local target_dir = "target/bundle"
    local os_name = ffi.os

    if os_name == "OSX" then
        local macos_bundle_dir = target_dir .. "/macos/" .. app_name .. ".app"
        local dmg_dir = target_dir .. "/dmg"
        local macos_dir = macos_bundle_dir .. "/Contents/MacOS"
        local res_dir = macos_bundle_dir .. "/Contents/Resources"

        info("Bundling macOS App...")
        exec("mkdir -p " .. shell_quote(macos_dir))
        exec("mkdir -p " .. shell_quote(res_dir))
        exec("mkdir -p " .. shell_quote(dmg_dir))

        exec("cp " .. shell_quote(bin_path) .. " " .. shell_quote(macos_dir .. "/" .. app_name))
        if file_exists("build/libtaurilua.dylib") then
            exec("cp build/libtaurilua.dylib " .. shell_quote(macos_dir .. "/"))
        end
        exec("cp -r " .. shell_quote(frontend_dist) .. " " .. shell_quote(res_dir .. "/"))

        local rocks_tree = get_rocks_tree_dir()
        if file_exists(rocks_tree) and (file_exists(rocks_tree .. "/share") or file_exists(rocks_tree .. "/lib")) then
            info("Packaging LuaRocks dependencies into macOS App bundle...")
            copy_dir_recursive(rocks_tree, res_dir .. "/rocks")
        end

        -- Info.plist
        local plist = string.format([[<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>%s</string>
    <key>CFBundleIdentifier</key>
    <string>%s</string>
    <key>CFBundleName</key>
    <string>%s</string>
    <key>CFBundleDisplayName</key>
    <string>%s</string>
    <key>CFBundleVersion</key>
    <string>%s</string>
    <key>CFBundleShortVersionString</key>
    <string>%s</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleIconFile</key>
    <string>icon</string>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>]], app_name, identifier, app_name, app_name, version, version)

        if file_exists("src-tauri/icons/icon.icns") then
            exec("cp src-tauri/icons/icon.icns " .. shell_quote(res_dir .. "/icon.icns"))
        end
        write_file(macos_bundle_dir .. "/Contents/Info.plist", plist)

        local sign_identity = os.getenv("APPLE_SIGNING_IDENTITY") or "-"
        info("Signing macOS application bundle with identity: " .. sign_identity)
        -- Sign nested dynamic libraries first (without --deep)
        if file_exists(macos_dir .. "/libtaurilua.dylib") then
            exec("codesign --force --sign " .. shell_quote(sign_identity) .. " " .. shell_quote(macos_dir .. "/libtaurilua.dylib") .. " 2>/dev/null || true")
        end
        local p_dylibs = io.popen("find " .. shell_quote(res_dir) .. " -type f \\( -name '*.dylib' -o -name '*.so' \\) 2>/dev/null")
        if p_dylibs then
            for dylib_path in p_dylibs:lines() do
                if dylib_path ~= "" then
                    exec("codesign --force --sign " .. shell_quote(sign_identity) .. " " .. shell_quote(dylib_path) .. " 2>/dev/null || true")
                end
            end
            p_dylibs:close()
        end

        -- Sign main bundle with Hardened Runtime and entitlements (avoiding deprecated --deep)
        local sign_opts = "--force --options runtime"
        if file_exists("src-tauri/entitlements.plist") then
            sign_opts = sign_opts .. " --entitlements " .. shell_quote("src-tauri/entitlements.plist")
        elseif file_exists(ENGINE_ROOT .. "/src-tauri/entitlements.plist") then
            sign_opts = sign_opts .. " --entitlements " .. shell_quote(ENGINE_ROOT .. "/src-tauri/entitlements.plist")
        end
        exec("codesign " .. sign_opts .. " --sign " .. shell_quote(sign_identity) .. " " .. shell_quote(macos_bundle_dir) .. " 2>/dev/null || codesign --force --sign - " .. shell_quote(macos_bundle_dir) .. " 2>/dev/null || true")

        local dmg_file = dmg_dir .. "/" .. app_name .. "_" .. version .. "_x64.dmg"
        info("Creating DMG disk image: " .. dmg_file)
        local diskutil_cmd = "diskutil image create from --volumeName " .. shell_quote(app_name) .. " --format UDZO " .. shell_quote(macos_bundle_dir) .. " " .. shell_quote(dmg_file) .. " > /dev/null 2>&1"
        if os.execute(diskutil_cmd) ~= 0 then
            exec("hdiutil create -volname " .. shell_quote(app_name) .. " -srcfolder " .. shell_quote(macos_bundle_dir) .. " -ov -format UDZO " .. shell_quote(dmg_file) .. " > /dev/null 2>&1")
        end

        success("Finished 2 bundles at:")
        io.stdout:write("        " .. colors.green .. macos_bundle_dir .. colors.reset .. "\n")
        io.stdout:write("        " .. colors.green .. dmg_file .. colors.reset .. "\n")

    elseif os_name == "Linux" then
        local deb_dir = target_dir .. "/deb/" .. app_name .. "_" .. version .. "_amd64"
        info("Bundling Debian (.deb)...")
        exec("mkdir -p " .. shell_quote(deb_dir .. "/DEBIAN"))
        exec("mkdir -p " .. shell_quote(deb_dir .. "/usr/bin"))
        exec("mkdir -p " .. shell_quote(deb_dir .. "/usr/share/" .. app_name .. "/" .. frontend_dist))

        exec("cp " .. shell_quote(bin_path) .. " " .. shell_quote(deb_dir .. "/usr/bin/" .. app_name))
        if file_exists("build/libtaurilua.so") then
            exec("cp build/libtaurilua.so " .. shell_quote(deb_dir .. "/usr/bin/") .. " 2>/dev/null || true")
        end
        exec("cp -r " .. shell_quote(frontend_dist) .. "/* " .. shell_quote(deb_dir .. "/usr/share/" .. app_name .. "/" .. frontend_dist .. "/"))

        local rocks_tree = get_rocks_tree_dir()
        if file_exists(rocks_tree) and (file_exists(rocks_tree .. "/share") or file_exists(rocks_tree .. "/lib")) then
            info("Packaging LuaRocks dependencies into Debian package...")
            local rocks_dest = deb_dir .. "/usr/share/" .. app_name .. "/rocks"
            copy_dir_recursive(rocks_tree, rocks_dest)
        end

        local control = string.format([[Package: %s
Version: %s
Section: utils
Priority: optional
Architecture: amd64
Maintainer: Developer <dev@example.com>
Description: %s built with Tauri-Lua
]], app_name, version, app_name)

        write_file(deb_dir .. "/DEBIAN/control", control)
        exec("dpkg-deb --build " .. shell_quote(deb_dir) .. " " .. shell_quote(target_dir .. "/deb") .. " > /dev/null")
        success("Finished bundle at " .. target_dir .. "/deb")

    elseif os_name == "Windows" then
        local win_dir = target_dir .. "\\windows\\" .. app_name .. "-standalone"
        info("Bundling Windows Standalone...")
        exec('mkdir "' .. win_dir .. '" 2>NUL || rem')
        exec('copy build\\Release\\tauri_app.exe "' .. win_dir .. '\\" 2>NUL || copy build\\tauri_app.exe "' .. win_dir .. '\\"')
        exec('copy build\\Release\\taurilua.dll "' .. win_dir .. '\\" 2>NUL || copy build\\taurilua.dll "' .. win_dir .. '\\" 2>NUL || rem')
        -- Bundle LuaJIT DLL (lua51.dll / luajit.dll) so the standalone runs on machines without a developer toolchain
        exec('copy build\\Release\\lua51.dll "' .. win_dir .. '\\" 2>NUL || copy build\\lua51.dll "' .. win_dir .. '\\" 2>NUL || copy build\\Release\\luajit.dll "' .. win_dir .. '\\" 2>NUL || copy build\\luajit.dll "' .. win_dir .. '\\" 2>NUL || rem')
        local luajit_env = os.getenv("LUAJIT_ROOT")
        if luajit_env then
            exec('copy "' .. luajit_env .. '\\bin\\lua51.dll" "' .. win_dir .. '\\" 2>NUL || copy "' .. luajit_env .. '\\lua51.dll" "' .. win_dir .. '\\" 2>NUL || rem')
        end
        exec('xcopy /E /I "' .. frontend_dist .. '" "' .. win_dir .. '\\' .. frontend_dist .. '"')

        local rocks_tree = get_rocks_tree_dir()
        if file_exists(rocks_tree) and (file_exists(rocks_tree .. "\\share") or file_exists(rocks_tree .. "\\lib") or file_exists(rocks_tree .. "/share") or file_exists(rocks_tree .. "/lib")) then
            info("Packaging LuaRocks dependencies into Windows standalone bundle...")
            copy_dir_recursive(rocks_tree, win_dir .. "\\rocks")
        end

        success("Finished bundle at " .. win_dir)
    end
end

-- ==========================================================
-- Command: CREATE (Interactive Template Scaffolder)
-- ==========================================================
local function prompt_line(msg, default_val)
    if default_val then
        io.stdout:write(colors.cyan .. "? " .. colors.reset .. colors.bold .. msg .. colors.reset .. " " .. colors.dim .. "(" .. default_val .. "): " .. colors.reset)
    else
        io.stdout:write(colors.cyan .. "? " .. colors.reset .. colors.bold .. msg .. colors.reset .. ": ")
    end
    io.stdout:flush()
    local line = io.stdin:read("*l")
    if not line or line:match("^%s*$") then
        return default_val
    end
    return line:match("^%s*(.-)%s*$")
end

local function prompt_choice(msg, options, default_idx)
    io.stdout:write(colors.cyan .. "? " .. colors.reset .. colors.bold .. msg .. colors.reset .. "\n")
    for idx, opt in ipairs(options) do
        local marker = (idx == default_idx) and (colors.green .. ">" .. colors.reset) or " "
        io.stdout:write(string.format("  %s %s%d)%s %s\n", marker, colors.dim, idx, colors.reset, opt.name))
    end
    io.stdout:write(colors.dim .. "  Choice [1-" .. #options .. "] (default " .. default_idx .. "): " .. colors.reset)
    io.stdout:flush()
    local line = io.stdin:read("*l")
    local chosen = tonumber(line)
    if not chosen or chosen < 1 or chosen > #options then
        return options[default_idx].value
    end
    return options[chosen].value
end

local function prompt_bool(msg, default_val)
    local hint = default_val and "(Y/n)" or "(y/N)"
    io.stdout:write(colors.cyan .. "? " .. colors.reset .. colors.bold .. msg .. colors.reset .. " " .. colors.dim .. hint .. ": " .. colors.reset)
    io.stdout:flush()
    local line = io.stdin:read("*l")
    if not line or line:match("^%s*$") then
        return default_val
    end
    line = line:lower():match("^%s*(.-)%s*$")
    return (line == "y" or line == "yes" or line == "s" or line == "si")
end

local function cmd_create(args)
    io.stdout:write("\n" .. colors.cyan .. colors.bold .. "  ⚡️ Tauri-Lua Create App" .. colors.reset .. "\n\n")

    local app_name = nil
    local template = nil
    local use_tailwind = nil
    local package_manager = nil

    local i = 1
    while i <= #args do
        local a = args[i]
        if a == "--template" or a == "-t" then
            template = args[i + 1]
            i = i + 2
        elseif a == "--tailwind" then
            use_tailwind = true
            i = i + 1
        elseif a == "--no-tailwind" then
            use_tailwind = false
            i = i + 1
        elseif a == "--pm" or a == "--package-manager" then
            package_manager = args[i + 1]
            i = i + 2
        elseif not a:match("^%-") and not app_name then
            app_name = a
            i = i + 1
        else
            i = i + 1
        end
    end

    if not app_name or app_name == "" then
        app_name = prompt_line("Project name", "my-tauri-app")
    end
    if not app_name or app_name == "" then app_name = "my-tauri-app" end
    app_name = app_name:gsub("[^%w%-_]", "-"):lower()

    if file_exists(app_name) then
        error("Target directory `" .. app_name .. "` already exists! Please choose another name.")
    end

    if not template or template == "" then
        local t_opts = {
            { name = "Vanilla (HTML5 / JS / CSS - Zero build step, instant start)", value = "vanilla" },
            { name = "React (TypeScript + Vite)", value = "react" },
            { name = "Vue 3 (TypeScript + Vite)", value = "vue" },
            { name = "Svelte (TypeScript + Vite)", value = "svelte" }
        }
        template = prompt_choice("Select your frontend framework:", t_opts, 1)
    end
    template = template:lower()
    if template ~= "vanilla" and template ~= "react" and template ~= "vue" and template ~= "svelte" then
        template = "vanilla"
    end

    if use_tailwind == nil then
        use_tailwind = prompt_bool("Configure Tailwind CSS?", false)
    end

    if template ~= "vanilla" and (not package_manager or package_manager == "") then
        local pm_opts = {
            { name = "npm", value = "npm" },
            { name = "pnpm", value = "pnpm" },
            { name = "bun", value = "bun" },
            { name = "yarn", value = "yarn" }
        }
        package_manager = prompt_choice("Select package manager:", pm_opts, 1)
    end
    if not package_manager or package_manager == "" then
        package_manager = "npm"
    end

    info("Scaffolding project `" .. app_name .. "` (" .. template .. ", " .. (use_tailwind and "Tailwind CSS" or "Standard CSS") .. ")...")

    exec("mkdir -p " .. shell_quote(app_name .. "/src-tauri/src"))
    exec("mkdir -p " .. shell_quote(app_name .. "/src-tauri/icons"))
    exec("mkdir -p " .. shell_quote(app_name .. "/src"))

    -- Copy icons
    if file_exists(ENGINE_ROOT .. "/src-tauri/icons") then
        exec("cp -r " .. shell_quote(ENGINE_ROOT .. "/src-tauri/icons") .. "/* " .. shell_quote(app_name .. "/src-tauri/icons/") .. " 2>/dev/null || true")
    end

    -- 1. Backend: main.lua
    local main_lua = [=[local Tauri = require("tauri")

-- 1. Initialize application with configuration
local conf = {}
if __EMBEDDED_CONFIG_JSON__ then
    conf = Tauri.json.decode(__EMBEDDED_CONFIG_JSON__) or {}
end
local app = Tauri.init(conf)

-- 2. State management
if not app.state:has("counter") then
    app.state:set("counter", 0)
end

-- 3. Register standard IPC command
app:register("greet", function(payload)
    local name = (payload and payload.name and payload.name ~= "") and payload.name or "Developer"
    local count = app.state:update("counter", function(c)
        return (c or 0) + 1
    end)

    return {
        greeting = "Hello " .. name .. "! ⚡️",
        invocations = count,
        engine = "LuaJIT " .. (jit and jit.version or "2.1"),
        workerId = __ISOLATE_WORKER_ID__ or 0
    }
end)

-- 4. Register real-time streaming channel
app:register("stream_countdown", function(payload, caller, channel)
    local ch = channel or (payload and payload.onProgress)
    local steps = tonumber(payload and payload.steps) or 5

    for i = steps, 1, -1 do
        if ch then
            ch:send({
                step = i,
                total = steps,
                progress = math.floor(((steps - i + 1) / steps) * 100),
                message = "Executing step #" .. (steps - i + 1) .. " of " .. steps .. "..."
            })
        end
        Tauri.sleep(200)
    end

    return { status = "success", message = "All pipeline steps executed cleanly!" }
end)

-- 5. Launch main window
app:create_main_window()
]=]
    write_file(app_name .. "/src-tauri/src/main.lua", main_lua)

    -- 2. Backend: tauri.conf.json
    local tauri_conf = {
        ["$schema"] = "../schemas/tauri-lua.schema.json",
        productName = app_name,
        version = "0.1.0",
        identifier = "com." .. app_name .. ".app",
        build = {},
        app = {
            windows = {
                {
                    title = app_name,
                    width = 1024,
                    height = 720,
                    resizable = true
                }
            },
            security = {
                capabilities = {
                    "core:default",
                    "fs:read-text-file",
                    "fs:write-text-file",
                    "dialog:open",
                    "dialog:message"
                }
            }
        }
    }

    if template == "vanilla" then
        tauri_conf.build.frontendDist = "../src"
        tauri_conf.build.devUrl = ""
    else
        tauri_conf.build.beforeDevCommand = package_manager .. " run dev"
        tauri_conf.build.devUrl = "http://localhost:5173"
        tauri_conf.build.beforeBuildCommand = package_manager .. " run build"
        tauri_conf.build.frontendDist = "../dist"
    end
    write_file(app_name .. "/src-tauri/tauri.conf.json", json.encode(tauri_conf))

    -- Shared CSS styling for rich aesthetics
    local shared_css = [=[
:root {
  --bg-primary: #0a0e17;
  --bg-card: rgba(18, 24, 38, 0.7);
  --border-card: rgba(255, 255, 255, 0.08);
  --accent-cyan: #38bdf8;
  --accent-indigo: #818cf8;
  --accent-purple: #c084fc;
  --text-main: #f8fafc;
  --text-dim: #94a3b8;
}
* { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
body {
  background: var(--bg-primary);
  color: var(--text-main);
  min-height: 100vh;
  display: flex;
  align-items: center;
  justify-content: center;
  overflow-x: hidden;
  position: relative;
  user-select: none;
}
.ambient-glow {
  position: absolute;
  width: 500px;
  height: 500px;
  background: radial-gradient(circle, rgba(56, 189, 248, 0.15) 0%, rgba(129, 140, 248, 0.08) 50%, transparent 70%);
  top: 50%;
  left: 50%;
  transform: translate(-50%, -50%);
  pointer-events: none;
  z-index: 0;
  filter: blur(40px);
}
.container {
  position: relative;
  z-index: 1;
  max-width: 840px;
  width: 90%;
  margin: 2rem auto;
  display: flex;
  flex-direction: column;
  gap: 2rem;
}
.header { text-align: center; }
.badge-row { display: flex; justify-content: center; gap: 0.75rem; margin-bottom: 1rem; }
.badge {
  font-size: 0.75rem;
  font-weight: 700;
  padding: 0.35rem 0.85rem;
  border-radius: 9999px;
  letter-spacing: 0.05em;
  text-transform: uppercase;
}
.badge-primary { background: rgba(56, 189, 248, 0.12); color: var(--accent-cyan); border: 1px solid rgba(56, 189, 248, 0.25); }
.badge-success { background: rgba(52, 211, 153, 0.12); color: #34d399; border: 1px solid rgba(52, 211, 153, 0.25); display: flex; align-items: center; gap: 0.4rem; }
.dot { width: 6px; height: 6px; border-radius: 50%; background: #34d399; box-shadow: 0 0 8px #34d399; }
.title {
  font-size: 2.25rem;
  font-weight: 800;
  background: linear-gradient(135deg, #ffffff 0%, #cbd5e1 50%, #94a3b8 100%);
  -webkit-background-clip: text;
  -webkit-text-fill-color: transparent;
  margin-bottom: 0.5rem;
}
.subtitle { color: var(--text-dim); font-size: 1rem; }
.grid { display: grid; grid-template-columns: 1fr 1fr; gap: 1.5rem; }
@media (max-width: 700px) { .grid { grid-template-columns: 1fr; } }
.card {
  background: var(--bg-card);
  backdrop-filter: blur(16px);
  -webkit-backdrop-filter: blur(16px);
  border: 1px solid var(--border-card);
  border-radius: 20px;
  padding: 1.75rem;
  display: flex;
  flex-direction: column;
  gap: 1.25rem;
  box-shadow: 0 20px 35px -10px rgba(0, 0, 0, 0.5);
  transition: transform 0.2s, border-color 0.2s;
}
.card:hover { transform: translateY(-3px); border-color: rgba(255, 255, 255, 0.18); }
.card-header { display: flex; align-items: center; gap: 0.9rem; }
.card-icon {
  width: 44px;
  height: 44px;
  border-radius: 12px;
  background: rgba(255, 255, 255, 0.05);
  display: flex;
  align-items: center;
  justify-content: center;
  font-size: 1.3rem;
  border: 1px solid rgba(255, 255, 255, 0.08);
}
.card-header h3 { font-size: 1.15rem; font-weight: 700; color: #fff; }
.card-header p { font-size: 0.8rem; color: var(--text-dim); }
.form-group { display: flex; flex-direction: column; gap: 0.75rem; }
input[type="text"] {
  background: rgba(10, 14, 23, 0.8);
  border: 1px solid var(--border-card);
  border-radius: 10px;
  padding: 0.75rem 1rem;
  color: #fff;
  font-size: 0.95rem;
  outline: none;
  transition: border-color 0.2s;
}
input[type="text"]:focus { border-color: var(--accent-cyan); }
.btn {
  border: none;
  border-radius: 10px;
  padding: 0.75rem 1.25rem;
  font-weight: 700;
  font-size: 0.95rem;
  cursor: pointer;
  transition: transform 0.15s, opacity 0.15s, box-shadow 0.15s;
}
.btn:active { transform: scale(0.97); }
.btn-primary {
  background: linear-gradient(135deg, var(--accent-cyan), var(--accent-indigo));
  color: #0a0e17;
  box-shadow: 0 4px 15px rgba(56, 189, 248, 0.25);
}
.btn-primary:hover { opacity: 0.92; box-shadow: 0 6px 20px rgba(56, 189, 248, 0.35); }
.btn-accent {
  background: linear-gradient(135deg, var(--accent-indigo), var(--accent-purple));
  color: #ffffff;
  box-shadow: 0 4px 15px rgba(129, 140, 248, 0.25);
}
.btn-accent:hover { opacity: 0.92; box-shadow: 0 6px 20px rgba(129, 140, 248, 0.35); }
.output-box {
  background: rgba(10, 14, 23, 0.9);
  border: 1px solid rgba(56, 189, 248, 0.25);
  border-radius: 10px;
  padding: 0.85rem;
  font-size: 0.85rem;
  color: var(--accent-cyan);
  line-height: 1.5;
  word-break: break-all;
}
.hidden { display: none; }
.progress-container { display: flex; flex-direction: column; gap: 0.5rem; margin-top: auto; }
.progress-bar {
  width: 100%;
  height: 8px;
  background: rgba(255, 255, 255, 0.08);
  border-radius: 9999px;
  overflow: hidden;
}
.progress-fill {
  height: 100%;
  background: linear-gradient(90deg, var(--accent-indigo), var(--accent-cyan));
  border-radius: 9999px;
  transition: width 0.25s cubic-bezier(0.4, 0, 0.2, 1);
}
.progress-meta { display: flex; justify-content: space-between; font-size: 0.8rem; color: var(--text-dim); }
.footer {
  display: flex;
  justify-content: center;
  align-items: center;
  gap: 0.75rem;
  font-size: 0.8rem;
  color: #64748b;
  margin-top: 1rem;
}
]=]

    -- 3. Frontend Scaffolding
    if template == "vanilla" then
        local vanilla_html = [=[<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>]=] .. app_name .. [=[</title>
  <link rel="stylesheet" href="style.css">
</head>
<body>
  <div class="ambient-glow"></div>
  <div class="container">
    <header class="header">
      <div class="badge-row">
        <span class="badge badge-primary">⚡️ Tauri-Lua</span>
        <span class="badge badge-success"><span class="dot"></span> LuaJIT Engine</span>
      </div>
      <h1 class="title">]=] .. app_name .. [=[</h1>
      <p class="subtitle">Vanilla frontend powered by a blazing fast LuaJIT C++17 runtime.</p>
    </header>

    <div class="grid">
      <div class="card">
        <div class="card-header">
          <div class="card-icon">💬</div>
          <div>
            <h3>IPC Invoke</h3>
            <p>Direct C++ / LuaJIT bridge dispatch</p>
          </div>
        </div>
        <div class="form-group">
          <input type="text" id="name-input" placeholder="Enter your name..." value="Developer" />
          <button id="greet-btn" class="btn btn-primary">Invoke Greet</button>
        </div>
        <div id="greet-output" class="output-box hidden"></div>
      </div>

      <div class="card">
        <div class="card-header">
          <div class="card-icon">🌊</div>
          <div>
            <h3>Channel Streaming</h3>
            <p>Real-time bidirectional streams</p>
          </div>
        </div>
        <div class="form-group">
          <button id="stream-btn" class="btn btn-accent">Start Stream Pipeline</button>
        </div>
        <div class="progress-container">
          <div class="progress-bar">
            <div id="progress-fill" class="progress-fill" style="width: 0%"></div>
          </div>
          <div class="progress-meta">
            <span id="stream-status">Idle</span>
            <span id="stream-pct">0%</span>
          </div>
        </div>
      </div>
    </div>

    <footer class="footer">
      <span>Zero Node.js runtime required</span>
      <span>•</span>
      <span>Hot-Reload Active</span>
    </footer>
  </div>

  <script src="main.js"></script>
</body>
</html>]=]
        write_file(app_name .. "/src/index.html", vanilla_html)
        write_file(app_name .. "/src/style.css", shared_css)

        local vanilla_js = [=[
const greetBtn = document.getElementById('greet-btn');
const nameInput = document.getElementById('name-input');
const greetOutput = document.getElementById('greet-output');

const streamBtn = document.getElementById('stream-btn');
const progressFill = document.getElementById('progress-fill');
const streamStatus = document.getElementById('stream-status');
const streamPct = document.getElementById('stream-pct');

greetBtn.addEventListener('click', async () => {
  const name = nameInput.value.trim() || 'Developer';
  try {
    greetBtn.textContent = 'Invoking...';
    const res = await window.__TAURI__.invoke('greet', { name });
    greetOutput.replaceChildren();
    const strong = document.createElement('strong');
    strong.textContent = res.greeting || '';
    const br = document.createElement('br');
    const small = document.createElement('small');
    small.style.color = '#94a3b8';
    small.textContent = `Worker #${res.workerId} | ${res.engine} | Call #${res.invocations}`;
    greetOutput.appendChild(strong);
    greetOutput.appendChild(br);
    greetOutput.appendChild(small);
    greetOutput.classList.remove('hidden');
  } catch (err) {
    greetOutput.classList.remove('hidden');
    greetOutput.textContent = 'Error: ' + err;
  } finally {
    greetBtn.textContent = 'Invoke Greet';
  }
});

streamBtn.addEventListener('click', async () => {
  streamBtn.disabled = true;
  streamBtn.textContent = 'Streaming Active...';
  progressFill.style.width = '0%';
  streamPct.textContent = '0%';
  streamStatus.textContent = 'Initializing stream channel...';

  try {
    const channel = new window.__TAURI__.Channel((data) => {
      progressFill.style.width = data.progress + '%';
      streamPct.textContent = data.progress + '%';
      streamStatus.textContent = data.message;
    });

    const result = await window.__TAURI__.invoke('stream_countdown', {
      steps: 5,
      onProgress: channel
    });

    streamStatus.textContent = result.message;
    progressFill.style.width = '100%';
    streamPct.textContent = '100%';
  } catch (err) {
    streamStatus.textContent = 'Stream error: ' + err;
  } finally {
    streamBtn.disabled = false;
    streamBtn.textContent = 'Start Stream Pipeline';
  }
});
]=]
        write_file(app_name .. "/src/main.js", vanilla_js)

        local vanilla_pkg = {
            name = app_name,
            version = "0.1.0",
            private = true,
            scripts = {
                ["tauri:dev"] = "./lua-tauri dev",
                ["tauri:build"] = "./lua-tauri build"
            }
        }
        write_file(app_name .. "/package.json", json.encode(vanilla_pkg))

    elseif template == "react" then
        local react_pkg = {
            name = app_name,
            private = true,
            version = "0.1.0",
            type = "module",
            scripts = {
                dev = "vite",
                build = "tsc && vite build",
                ["tauri:dev"] = "./lua-tauri dev",
                ["tauri:build"] = "./lua-tauri build"
            },
            dependencies = {
                react = "^18.3.1",
                ["react-dom"] = "^18.3.1"
            },
            devDependencies = {
                ["@types/react"] = "^18.3.5",
                ["@types/react-dom"] = "^18.3.0",
                ["@vitejs/plugin-react"] = "^4.3.1",
                typescript = "^5.5.3",
                vite = "^5.4.2"
            }
        }
        if use_tailwind then
            react_pkg.devDependencies["tailwindcss"] = "^3.4.10"
            react_pkg.devDependencies["postcss"] = "^8.4.41"
            react_pkg.devDependencies["autoprefixer"] = "^10.4.20"
        end
        write_file(app_name .. "/package.json", json.encode(react_pkg))

        local vite_conf = [=[import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

export default defineConfig({
  plugins: [react()],
  clearScreen: false,
  server: {
    port: 5173,
    strictPort: true,
  },
});
]=]
        write_file(app_name .. "/vite.config.ts", vite_conf)

        local tsconfig = [=[{
  "compilerOptions": {
    "target": "ES2020",
    "useDefineForClassFields": true,
    "lib": ["ES2020", "DOM", "DOM.Iterable"],
    "module": "ESNext",
    "skipLibCheck": true,
    "moduleResolution": "bundler",
    "allowImportingTsExtensions": true,
    "resolveJsonModule": true,
    "isolatedModules": true,
    "noEmit": true,
    "jsx": "react-jsx",
    "strict": true,
    "noUnusedLocals": true,
    "noUnusedParameters": true,
    "noFallthroughCasesInSwitch": true
  },
  "include": ["src"]
}
]=]
        write_file(app_name .. "/tsconfig.json", tsconfig)

        local index_html = [=[<!doctype html>
<html lang="en">
  <head>
    <meta charset="UTF-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1.0" />
    <title>]=] .. app_name .. [=[</title>
  </head>
  <body>
    <div id="root"></div>
    <script type="module" src="/src/main.tsx"></script>
  </body>
</html>
]=]
        write_file(app_name .. "/index.html", index_html)

        local main_tsx = [=[import React from 'react';
import ReactDOM from 'react-dom/client';
import App from './App';
import './index.css';

ReactDOM.createRoot(document.getElementById('root') as HTMLElement).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>
);
]=]
        write_file(app_name .. "/src/main.tsx", main_tsx)

        local app_tsx = [=[import React, { useState } from 'react';

declare global {
  interface Window {
    __TAURI__: {
      invoke: (cmd: string, args?: any) => Promise<any>;
      Channel: new (cb: (msg: any) => void) => any;
    };
  }
}

export function App() {
  const [name, setName] = useState('Developer');
  const [greetRes, setGreetRes] = useState<any>(null);
  const [loading, setLoading] = useState(false);
  const [streamProgress, setStreamProgress] = useState(0);
  const [streamMsg, setStreamMsg] = useState('Idle');
  const [streaming, setStreaming] = useState(false);

  const handleGreet = async () => {
    setLoading(true);
    try {
      const res = await window.__TAURI__.invoke('greet', { name });
      setGreetRes(res);
    } catch (e: any) {
      alert('IPC Error: ' + e);
    } finally {
      setLoading(false);
    }
  };

  const handleStream = async () => {
    setStreaming(true);
    setStreamProgress(0);
    setStreamMsg('Starting stream...');
    try {
      const ch = new window.__TAURI__.Channel((data: any) => {
        setStreamProgress(data.progress);
        setStreamMsg(data.message);
      });
      const res = await window.__TAURI__.invoke('stream_countdown', { steps: 5, onProgress: ch });
      setStreamMsg(res.message);
    } catch (e: any) {
      setStreamMsg('Error: ' + e);
    } finally {
      setStreaming(false);
    }
  };

  return (
    <div className="container">
      <header className="header">
        <div className="badge-row">
          <span className="badge badge-primary">⚡️ React + Tauri-Lua</span>
          <span className="badge badge-success"><span className="dot"></span> LuaJIT Engine</span>
        </div>
        <h1 className="title">]=] .. app_name .. [=[</h1>
        <p className="subtitle">Vite + React powered by high-performance LuaJIT backend.</p>
      </header>

      <div className="grid">
        <div className="card">
          <div className="card-header">
            <div className="card-icon">💬</div>
            <div>
              <h3>IPC Invoke</h3>
              <p>Direct call to Lua backend</p>
            </div>
          </div>
          <div className="form-group">
            <input type="text" value={name} onChange={(e) => setName(e.target.value)} placeholder="Your name..." />
            <button className="btn btn-primary" onClick={handleGreet} disabled={loading}>
              {loading ? 'Invoking...' : 'Invoke Greet'}
            </button>
          </div>
          {greetRes && (
            <div className="output-box">
              <strong>{greetRes.greeting}</strong><br />
              <small>Worker #{greetRes.workerId} | Call #{greetRes.invocations}</small>
            </div>
          )}
        </div>

        <div className="card">
          <div className="card-header">
            <div className="card-icon">🌊</div>
            <div>
              <h3>Channel Streaming</h3>
              <p>Real-time bidirectional stream</p>
            </div>
          </div>
          <div className="form-group">
            <button className="btn btn-accent" onClick={handleStream} disabled={streaming}>
              {streaming ? 'Streaming...' : 'Start Stream Pipeline'}
            </button>
          </div>
          <div className="progress-container">
            <div className="progress-bar">
              <div className="progress-fill" style={{ width: `${streamProgress}%` }}></div>
            </div>
            <div className="progress-meta">
              <span>{streamMsg}</span>
              <span>{streamProgress}%</span>
            </div>
          </div>
        </div>
      </div>
    </div>
  );
}

export default App;
]=]
        write_file(app_name .. "/src/App.tsx", app_tsx)
        local tailwind_prefix = use_tailwind and "@tailwind base;\n@tailwind components;\n@tailwind utilities;\n\n" or ""
        write_file(app_name .. "/src/index.css", tailwind_prefix .. shared_css)

    elseif template == "vue" then
        local vue_pkg = {
            name = app_name,
            private = true,
            version = "0.1.0",
            type = "module",
            scripts = {
                dev = "vite",
                build = "vue-tsc && vite build",
                ["tauri:dev"] = "./lua-tauri dev",
                ["tauri:build"] = "./lua-tauri build"
            },
            dependencies = {
                vue = "^3.4.38"
            },
            devDependencies = {
                ["@vitejs/plugin-vue"] = "^5.1.2",
                typescript = "^5.5.3",
                vite = "^5.4.2",
                ["vue-tsc"] = "^2.1.4"
            }
        }
        if use_tailwind then
            vue_pkg.devDependencies["tailwindcss"] = "^3.4.10"
            vue_pkg.devDependencies["postcss"] = "^8.4.41"
            vue_pkg.devDependencies["autoprefixer"] = "^10.4.20"
        end
        write_file(app_name .. "/package.json", json.encode(vue_pkg))

        local vite_conf = [=[import { defineConfig } from 'vite';
import vue from '@vitejs/plugin-vue';

export default defineConfig({
  plugins: [vue()],
  clearScreen: false,
  server: {
    port: 5173,
    strictPort: true,
  },
});
]=]
        write_file(app_name .. "/vite.config.ts", vite_conf)

        local index_html = [=[<!doctype html>
<html lang="en">
  <head>
    <meta charset="UTF-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1.0" />
    <title>]=] .. app_name .. [=[</title>
  </head>
  <body>
    <div id="app"></div>
    <script type="module" src="/src/main.ts"></script>
  </body>
</html>
]=]
        write_file(app_name .. "/index.html", index_html)

        local main_ts = [=[import { createApp } from 'vue';
import App from './App.vue';
import './style.css';

createApp(App).mount('#app');
]=]
        write_file(app_name .. "/src/main.ts", main_ts)

        local app_vue = [=[<script setup lang="ts">
import { ref } from 'vue';

const name = ref('Developer');
const greetRes = ref<any>(null);
const loading = ref(false);
const streamProgress = ref(0);
const streamMsg = ref('Idle');
const streaming = ref(false);

declare const window: any;

const handleGreet = async () => {
  loading.value = true;
  try {
    const res = await window.__TAURI__.invoke('greet', { name: name.value });
    greetRes.value = res;
  } catch (e: any) {
    alert('IPC Error: ' + e);
  } finally {
    loading.value = false;
  }
};

const handleStream = async () => {
  streaming.value = true;
  streamProgress.value = 0;
  streamMsg.value = 'Starting stream...';
  try {
    const ch = new window.__TAURI__.Channel((data: any) => {
      streamProgress.value = data.progress;
      streamMsg.value = data.message;
    });
    const res = await window.__TAURI__.invoke('stream_countdown', { steps: 5, onProgress: ch });
    streamMsg.value = res.message;
  } catch (e: any) {
    streamMsg.value = 'Error: ' + e;
  } finally {
    streaming.value = false;
  }
};
</script>

<template>
  <div class="container">
    <header class="header">
      <div class="badge-row">
        <span class="badge badge-primary">⚡️ Vue 3 + Tauri-Lua</span>
        <span class="badge badge-success"><span class="dot"></span> LuaJIT Engine</span>
      </div>
      <h1 class="title">]=] .. app_name .. [=[</h1>
      <p class="subtitle">Vite + Vue 3 powered by high-performance LuaJIT backend.</p>
    </header>

    <div class="grid">
      <div class="card">
        <div class="card-header">
          <div class="card-icon">💬</div>
          <div>
            <h3>IPC Invoke</h3>
            <p>Direct call to Lua backend</p>
          </div>
        </div>
        <div class="form-group">
          <input type="text" v-model="name" placeholder="Your name..." />
          <button class="btn btn-primary" @click="handleGreet" :disabled="loading">
            {{ loading ? 'Invoking...' : 'Invoke Greet' }}
          </button>
        </div>
        <div v-if="greetRes" class="output-box">
          <strong>{{ greetRes.greeting }}</strong><br />
          <small>Worker #{{ greetRes.workerId }} | Call #{{ greetRes.invocations }}</small>
        </div>
      </div>

      <div class="card">
        <div class="card-header">
          <div class="card-icon">🌊</div>
          <div>
            <h3>Channel Streaming</h3>
            <p>Real-time bidirectional stream</p>
          </div>
        </div>
        <div class="form-group">
          <button class="btn btn-accent" @click="handleStream" :disabled="streaming">
            {{ streaming ? 'Streaming...' : 'Start Stream Pipeline' }}
          </button>
        </div>
        <div class="progress-container">
          <div class="progress-bar">
            <div class="progress-fill" :style="{ width: streamProgress + '%' }"></div>
          </div>
          <div class="progress-meta">
            <span>{{ streamMsg }}</span>
            <span>{{ streamProgress }}%</span>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>
]=]
        write_file(app_name .. "/src/App.vue", app_vue)
        local tailwind_prefix = use_tailwind and "@tailwind base;\n@tailwind components;\n@tailwind utilities;\n\n" or ""
        write_file(app_name .. "/src/style.css", tailwind_prefix .. shared_css)

    elseif template == "svelte" then
        local svelte_pkg = {
            name = app_name,
            private = true,
            version = "0.1.0",
            type = "module",
            scripts = {
                dev = "vite",
                build = "vite build",
                ["tauri:dev"] = "./lua-tauri dev",
                ["tauri:build"] = "./lua-tauri build"
            },
            devDependencies = {
                ["@sveltejs/vite-plugin-svelte"] = "^3.1.2",
                svelte = "^4.2.19",
                typescript = "^5.5.3",
                vite = "^5.4.2"
            }
        }
        if use_tailwind then
            svelte_pkg.devDependencies["tailwindcss"] = "^3.4.10"
            svelte_pkg.devDependencies["postcss"] = "^8.4.41"
            svelte_pkg.devDependencies["autoprefixer"] = "^10.4.20"
        end
        write_file(app_name .. "/package.json", json.encode(svelte_pkg))

        local vite_conf = [=[import { defineConfig } from 'vite';
import { svelte, vitePreprocess } from '@sveltejs/vite-plugin-svelte';

export default defineConfig({
  plugins: [
    svelte({
      preprocess: vitePreprocess(),
    }),
  ],
  clearScreen: false,
  server: {
    port: 5173,
    strictPort: true,
  },
});
]=]
        write_file(app_name .. "/vite.config.ts", vite_conf)

        local index_html = [=[<!doctype html>
<html lang="en">
  <head>
    <meta charset="UTF-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1.0" />
    <title>]=] .. app_name .. [=[</title>
  </head>
  <body>
    <div id="app"></div>
    <script type="module" src="/src/main.ts"></script>
  </body>
</html>
]=]
        write_file(app_name .. "/index.html", index_html)

        local main_ts = [=[import './app.css';
import App from './App.svelte';

const app = new App({
  target: document.getElementById('app')!,
});

export default app;
]=]
        write_file(app_name .. "/src/main.ts", main_ts)

        local app_svelte = [=[<script lang="ts">
  let name = 'Developer';
  let greetRes: any = null;
  let loading = false;
  let streamProgress = 0;
  let streamMsg = 'Idle';
  let streaming = false;

  declare const window: any;

  async function handleGreet() {
    loading = true;
    try {
      greetRes = await window.__TAURI__.invoke('greet', { name });
    } catch (e: any) {
      alert('IPC Error: ' + e);
    } finally {
      loading = false;
    }
  }

  async function handleStream() {
    streaming = true;
    streamProgress = 0;
    streamMsg = 'Starting stream...';
    try {
      const ch = new window.__TAURI__.Channel((data: any) => {
        streamProgress = data.progress;
        streamMsg = data.message;
      });
      const res = await window.__TAURI__.invoke('stream_countdown', { steps: 5, onProgress: ch });
      streamMsg = res.message;
    } catch (e: any) {
      streamMsg = 'Error: ' + e;
    } finally {
      streaming = false;
    }
  }
</script>

<div class="container">
  <header class="header">
    <div class="badge-row">
      <span class="badge badge-primary">⚡️ Svelte + Tauri-Lua</span>
      <span class="badge badge-success"><span class="dot"></span> LuaJIT Engine</span>
    </div>
    <h1 class="title">]=] .. app_name .. [=[</h1>
    <p class="subtitle">Vite + Svelte powered by high-performance LuaJIT backend.</p>
  </header>

  <div class="grid">
    <div class="card">
      <div class="card-header">
        <div class="card-icon">💬</div>
        <div>
          <h3>IPC Invoke</h3>
          <p>Direct call to Lua backend</p>
        </div>
      </div>
      <div class="form-group">
        <input type="text" bind:value={name} placeholder="Your name..." />
        <button class="btn btn-primary" on:click={handleGreet} disabled={loading}>
          {loading ? 'Invoking...' : 'Invoke Greet'}
        </button>
      </div>
      {#if greetRes}
        <div class="output-box">
          <strong>{greetRes.greeting}</strong><br />
          <small>Worker #{greetRes.workerId} | Call #{greetRes.invocations}</small>
        </div>
      {/if}
    </div>

    <div class="card">
      <div class="card-header">
        <div class="card-icon">🌊</div>
        <div>
          <h3>Channel Streaming</h3>
          <p>Real-time bidirectional stream</p>
        </div>
      </div>
      <div class="form-group">
        <button class="btn btn-accent" on:click={handleStream} disabled={streaming}>
          {streaming ? 'Streaming...' : 'Start Stream Pipeline'}
        </button>
      </div>
      <div class="progress-container">
        <div class="progress-bar">
          <div class="progress-fill" style="width: {streamProgress}%"></div>
        </div>
        <div class="progress-meta">
          <span>{streamMsg}</span>
          <span>{streamProgress}%</span>
        </div>
      </div>
    </div>
  </div>
</div>
]=]
        write_file(app_name .. "/src/App.svelte", app_svelte)
        local tailwind_prefix = use_tailwind and "@tailwind base;\n@tailwind components;\n@tailwind utilities;\n\n" or ""
        write_file(app_name .. "/src/app.css", tailwind_prefix .. shared_css)
    end

    -- 4. Optional Tailwind configs
    if use_tailwind then
        local tailwind_config = [=[/** @type {import('tailwindcss').Config} */
export default {
  content: [
    "./index.html",
    "./src/**/*.{js,ts,jsx,tsx,vue,svelte}",
  ],
  theme: {
    extend: {},
  },
  plugins: [],
}
]=]
        write_file(app_name .. "/tailwind.config.js", tailwind_config)

        local postcss_config = [=[export default {
  plugins: {
    tailwindcss: {},
    autoprefixer: {},
  },
}
]=]
        write_file(app_name .. "/postcss.config.js", postcss_config)
    end

    -- 5. .gitignore
    local gitignore = [[node_modules/
dist/
build/
target/
.DS_Store
*.log
]]
    write_file(app_name .. "/.gitignore", gitignore)

    -- 6. lua-tauri local launcher script
    local launcher_script = [=[#!/usr/bin/env bash
set -e
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENGINE_PATH="${TAURI_LUA_ROOT}"

if [ -z "$ENGINE_PATH" ] || [ ! -d "$ENGINE_PATH" ]; then
    if [ -d "$PROJECT_DIR/../lua-tauri" ] && [ -f "$PROJECT_DIR/../lua-tauri/lua-tauri" ]; then
        ENGINE_PATH="$(cd "$PROJECT_DIR/../lua-tauri" && pwd)"
    elif [ -f "$PROJECT_DIR/../lua-tauri" ]; then
        ENGINE_PATH="$(cd "$PROJECT_DIR/.." && pwd)"
    fi
fi

if [ -z "$ENGINE_PATH" ] || [ ! -f "$ENGINE_PATH/lua-tauri" ]; then
    echo "Error: Could not locate Tauri-Lua engine. Set TAURI_LUA_ROOT or place project near engine."
    exit 1
fi

export TAURI_LUA_ROOT="$ENGINE_PATH"
exec "$ENGINE_PATH/lua-tauri" "$@"
]=]
    write_file(app_name .. "/lua-tauri", launcher_script)
    exec("chmod +x " .. shell_quote(app_name .. "/lua-tauri"))

    success("Created project `" .. app_name .. "` successfully!")
    io.stdout:write("\n" .. colors.bold .. "  Next steps:\n" .. colors.reset)
    io.stdout:write("    cd " .. app_name .. "\n")
    if template ~= "vanilla" then
        io.stdout:write("    " .. package_manager .. " install\n")
    end
    io.stdout:write("    ./lua-tauri dev\n\n")
end

-- ==========================================================
-- Command: INIT
-- ==========================================================
local function cmd_init(args)
    info("Initializing new Tauri-Lua project structure...")

    exec("mkdir -p src-tauri/src")
    exec("mkdir -p src-tauri/icons")
    exec("mkdir -p src")

    local default_conf = {
        ["$schema"] = "../schemas/tauri-lua.schema.json",
        productName = "my-tauri-app",
        version = "0.1.0",
        identifier = "com.mytauriapp.dev",
        build = {
            frontendDist = "../src",
            devUrl = ""
        },
        app = {
            windows = {
                {
                    title = "My Tauri App",
                    width = 1024,
                    height = 720,
                    resizable = true
                }
            },
            security = {
                capabilities = {
                    "core:default",
                    "fs:read-text-file",
                    "dialog:open",
                    "dialog:message"
                }
            }
        }
    }

    if not file_exists("src-tauri/tauri.conf.json") then
        write_file("src-tauri/tauri.conf.json", json.encode(default_conf))
        success("Created src-tauri/tauri.conf.json")
    else
        warn("src-tauri/tauri.conf.json already exists, skipped.")
    end

    if not file_exists("src-tauri/src/main.lua") then
        local sample_main = [[local Tauri = require("tauri")

local conf = {}
if __EMBEDDED_CONFIG_JSON__ then
    conf = Tauri.json.decode(__EMBEDDED_CONFIG_JSON__) or {}
end

local app = Tauri.init(conf)

app:register("greet", function(payload)
    local name = payload and payload.name or "World"
    return "Hello " .. name .. "! Greetings from Lua backend."
end)

app:create_main_window()
]]
        write_file("src-tauri/src/main.lua", sample_main)
        success("Created src-tauri/src/main.lua")
    else
        warn("src-tauri/src/main.lua already exists, skipped.")
    end

    if not file_exists("src/index.html") then
        local sample_html = [[<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <title>My Tauri App</title>
    <style>
        body {
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
            background: #0f172a;
            color: #f8fafc;
            display: flex;
            align-items: center;
            justify-content: center;
            height: 100vh;
            margin: 0;
        }
        .card {
            background: rgba(30, 41, 59, 0.7);
            border: 1px solid rgba(255, 255, 255, 0.1);
            backdrop-filter: blur(12px);
            padding: 2.5rem;
            border-radius: 16px;
            text-align: center;
            box-shadow: 0 20px 25px -5px rgba(0, 0, 0, 0.5);
            max-width: 480px;
        }
        button {
            background: linear-gradient(135deg, #38bdf8, #818cf8);
            border: none;
            padding: 10px 20px;
            font-size: 15px;
            font-weight: 600;
            color: #0f172a;
            border-radius: 8px;
            cursor: pointer;
            transition: transform 0.15s, opacity 0.15s;
        }
        button:hover { opacity: 0.9; transform: scale(1.03); }
        #output { margin-top: 1.5rem; font-size: 16px; color: #38bdf8; min-height: 24px; }
    </style>
</head>
<body>
    <div class="card">
        <h1>Tauri-Lua</h1>
        <p>Native desktop app powered by LuaJIT.</p>
        <button id="btn">Call Backend Command</button>
        <div id="output"></div>
    </div>
    <script>
        document.getElementById('btn').addEventListener('click', async () => {
            const res = await window.__TAURI__.invoke('greet', { name: 'Developer' });
            document.getElementById('output').textContent = res;
        });
    </script>
</body>
</html>
]]
        write_file("src/index.html", sample_html)
        success("Created src/index.html")
    end

    success("Project initialized! Run `./lua-tauri dev` to start developing.")
end

-- ==========================================================
-- Command: ICON
-- ==========================================================
local function cmd_icon(args)
    local src_img = args[1]
    if not src_img or not file_exists(src_img) then
        error("Usage: lua-tauri icon <path/to/source-icon.png>")
    end

    info("Generating platform icons from " .. src_img)
    exec("mkdir -p src-tauri/icons")

    if ffi.os == "OSX" then
        local iconset_dir = "src-tauri/icons/icon.iconset"
        exec("rm -rf " .. shell_quote(iconset_dir))
        exec("mkdir -p " .. shell_quote(iconset_dir))
        local specs = {
            { name = "icon_16x16.png", size = 16 },
            { name = "icon_16x16@2x.png", size = 32 },
            { name = "icon_32x32.png", size = 32 },
            { name = "icon_32x32@2x.png", size = 64 },
            { name = "icon_128x128.png", size = 128 },
            { name = "icon_128x128@2x.png", size = 256 },
            { name = "icon_256x256.png", size = 256 },
            { name = "icon_256x256@2x.png", size = 512 },
            { name = "icon_512x512.png", size = 512 },
            { name = "icon_512x512@2x.png", size = 1024 },
        }
        for _, sp in ipairs(specs) do
            local out = iconset_dir .. "/" .. sp.name
            exec(string.format("sips -s format png -z %d %d %s --out %s > /dev/null 2>&1", sp.size, sp.size, shell_quote(src_img), shell_quote(out)))
        end
        exec("iconutil -c icns " .. shell_quote(iconset_dir) .. " -o src-tauri/icons/icon.icns")
        exec("rm -rf " .. shell_quote(iconset_dir))

        -- Also generate standard PNGs
        exec(string.format("sips -s format png -z 512 512 %s --out src-tauri/icons/icon.png > /dev/null 2>&1", shell_quote(src_img)))
        exec(string.format("sips -s format png -z 128 128 %s --out src-tauri/icons/128x128.png > /dev/null 2>&1", shell_quote(src_img)))
        exec(string.format("sips -s format png -z 32 32 %s --out src-tauri/icons/32x32.png > /dev/null 2>&1", shell_quote(src_img)))

        success("Generated src-tauri/icons/icon.icns and PNG icons successfully")
    else
        warn("Automated icon generation is currently supported natively on macOS via sips/iconutil.")
    end
end

local function cmd_clean(args)
    local clean_all = false
    if args and (args[1] == "--all" or args[1] == "-a" or args[1] == "--rocks") then
        clean_all = true
    end
    info("Cleaning build artifacts and release bundles...")
    remove_dir_recursive("build")
    remove_dir_recursive("target")
    if ffi.os == "OSX" then
        os.execute("rm -f .DS_Store **/.DS_Store 2>/dev/null")
    end
    if clean_all then
        local rocks_tree = get_rocks_tree_dir()
        if file_exists(rocks_tree) then
            info("Cleaning hermetic LuaRocks tree: " .. rocks_tree)
            remove_dir_recursive(rocks_tree)
        end
    end
    success("Workspace cleaned successfully.")
end

-- ==========================================================
-- CLI Main Entrypoint
-- ==========================================================
local function print_help()
    io.stdout:write([[
Tauri-Lua CLI (Fast, lightweight native desktop runtime)

USAGE:
    lua-tauri <SUBCOMMAND>

SUBCOMMANDS:
    create   Scaffold a new Tauri-Lua project (Vanilla, React, Vue, Svelte)
    dev      Start development server with live reload and HMR
    build    Build release application and generate platform packages (.app, .dmg, .deb)
    rocks    Manage hermetic LuaRocks dependencies (install, list, remove, clean, path)
    init     Initialize a new src-tauri/ configuration in current folder
    icon     Generate multi-platform icons from a source PNG
    clean    Remove build/ and target/ directories to reset workspace (use --all to clean .rocks)
    help     Print this help message

]])
end

if _G.TAURI_CLI_TEST_MODE then
    return {
        json = json,
        shell_quote = shell_quote,
        parse_rock_server_url = parse_rock_server_url,
        validate_rock_server = validate_rock_server,
        validate_rock_name = validate_rock_name,
        is_pinned_version = is_pinned_version,
        validate_rock_version = validate_rock_version,
        read_lockfile = read_lockfile,
        save_lockfile = save_lockfile,
        compute_file_checksum = compute_file_checksum,
        compute_composite_checksum = compute_composite_checksum,
        get_installed_rock_files = get_installed_rock_files,
        get_installed_rock_details = get_installed_rock_details,
        ensure_rocks_installed = ensure_rocks_installed,
    }
end

local cmd = arg and arg[1] or "help"
local subargs = {}
if arg then
    for i = 2, #arg do table.insert(subargs, arg[i]) end
end

if cmd == "create" then
    cmd_create(subargs)
elseif cmd == "dev" then
    cmd_dev(subargs)
elseif cmd == "build" then
    cmd_build(subargs)
elseif cmd == "rocks" then
    cmd_rocks(subargs)
elseif cmd == "init" then
    cmd_init(subargs)
elseif cmd == "icon" then
    cmd_icon(subargs)
elseif cmd == "clean" then
    cmd_clean(subargs)
elseif cmd == "help" or cmd == "--help" or cmd == "-h" then
    print_help()
else
    err("Unknown subcommand: " .. cmd)
    print_help()
    os.exit(1)
end
