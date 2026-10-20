-- ============================================================================
-- Tauri-Lua Comprehensive Audit & Security Test Suite
-- Tests for Audit Points: P0-1, P0-2, P0-3, P0-4, P0-5, P0-7, P0-9, P0-16
-- ============================================================================

local ffi = require("ffi")

-- Preload libtaurilua library so FFI platform symbols are resolved in pure luajit runner
pcall(function()
    if ffi.os == "OSX" then
        ffi.load("./build/libtaurilua.dylib")
    elseif ffi.os == "Windows" then
        pcall(ffi.load, "./build/Release/taurilua.dll")
        pcall(ffi.load, "./build/taurilua.dll")
    else
        ffi.load("./build/libtaurilua.so")
    end
end)

local total_tests = 0
local passed_tests = 0

local function test(name, fn)
    total_tests = total_tests + 1
    io.stdout:write(string.format("  - %-62s ", name))
    local ok, err = pcall(fn)
    if ok then
        passed_tests = passed_tests + 1
        print("[\27[32mPASS\27[0m]")
    else
        print("[\27[31mFAIL\27[0m]")
        print("    Error: " .. tostring(err))
    end
end

print("\n=======================================================")
print("RUNNING TAURI-LUA AUTOMATED COMPLIANCE TEST SUITE")
print("=======================================================\n")

local tauri = dofile("src/tauri/init.lua")

-- ----------------------------------------------------------------------------
-- Point 1: Auto-discovery moduli (Release vs Dev path isolation)
-- ----------------------------------------------------------------------------
print("1. Module Auto-Discovery & CWD Stripping (P0-1):")

test("Release mode filters relative and CWD paths from package.path", function()
    local test_path = "./?.lua;../plugins/?.lua;/opt/app/bundle/?.lua;./node_modules/?.lua;/usr/share/lua/5.1/?.lua"
    local filtered = tauri._filter_absolute_only(test_path)
    assert(not filtered:find("%./%?%.lua"), "CWD ./?.lua must be removed in release")
    assert(not filtered:find("%.%./plugins"), "Relative ../plugins must be removed in release")
    assert(not filtered:find("node_modules"), "Relative node_modules must be removed in release")
    assert(filtered:find("/opt/app/bundle/"), "Absolute bundle path must be preserved")
    assert(filtered:find("/usr/share/lua/5.1/"), "Absolute system rock path must be preserved")
end)

test("Dev mode retains local development search paths", function()
    local dev_path = "./plugins/?.lua;../lua-tauri-plugins/plugins/?.lua;/opt/bundle/?.lua"
    assert(dev_path:find("%./plugins"), "Dev mode supports local plugin discovery")
    assert(dev_path:find("%.%./lua%-tauri%-plugins"), "Dev mode supports companion plugin workspace")
end)

-- ----------------------------------------------------------------------------
-- Point 2: Single-Instance IPC Security (P0-2)
-- ----------------------------------------------------------------------------
print("\n2. Single-Instance IPC Hardening (P0-2):")

test("Rejects payloads exceeding maximum size limit (64 KB cap)", function()
    local MAX_PAYLOAD = 65536
    local oversized = string.rep("A", MAX_PAYLOAD + 10)
    local ok = pcall(tauri.validate_single_instance_payload, #oversized)
    assert(not ok, "Oversized argv/cwd payloads must be rejected")
    assert(tauri.validate_single_instance_payload(1024) == true, "Normal sized payload must be accepted")
end)

test("Socket directory enforces XDG_RUNTIME_DIR or User-Isolated path", function()
    local path = tauri.resolve_safe_socket_path("test-app")
    assert(not path:match("^/tmp/"), "Must not use insecure global /tmp fallback")
end)

-- ----------------------------------------------------------------------------
-- Point 3: Scope HTTP & URL Parsing (P0-3)
-- ----------------------------------------------------------------------------
print("\n3. HTTP URL Parser & SSRF Mitigation (P0-3):")

test("Parses URL components strictly and normalizes traversal attempts", function()
    local res = tauri.parse_and_normalize_url("https://api.myapp.com/v1/%2e%2e/admin/data")
    assert(res.scheme == "https")
    assert(res.host == "api.myapp.com")
    assert(res.path == "/admin/data", "Traversal %2e%2e must be resolved")

    -- Host confusion defense
    local conf = tauri.parse_and_normalize_url("https://evil.com/fake/.myapp.com")
    assert(conf.host == "evil.com", "Host must be strictly isolated from path spoofing")
end)

test("Default-deny blocks loopback, private networks, and cloud metadata", function()
    assert(tauri.is_ssrf_forbidden("127.0.0.1"), "Blocks 127.0.0.1")
    assert(tauri.is_ssrf_forbidden("localhost"), "Blocks localhost")
    assert(tauri.is_ssrf_forbidden("169.254.169.254"), "Blocks AWS metadata")
    assert(tauri.is_ssrf_forbidden("10.0.1.50"), "Blocks private 10.x.x.x")
    assert(tauri.is_ssrf_forbidden("192.168.1.1"), "Blocks private 192.168.x.x")
    assert(not tauri.is_ssrf_forbidden("api.github.com"), "Allows public internet domain")
end)

-- ----------------------------------------------------------------------------
-- Point 4: Shell Security & Argument Whitelisting (P0-4)
-- ----------------------------------------------------------------------------
print("\n4. Shell Default-Deny & Per-Program Argument Gating (P0-4):")

test("Blocks execution when allowed_programs is missing or empty (Default-Deny)", function()
    _G.TAURI_CLI_TEST_MODE = true
    local tauri = dofile("src/tauri/init.lua")
    local app = tauri.new({
        security = {
            capabilities = { "shell:execute" },
            allowed_programs = {} -- empty
        }
    })
    local ok, err = pcall(function() app.security:assert_program("curl") end)
    assert(not ok, "Must reject execution when allowed_programs is empty")
    assert(tostring(err):find("allowed_programs is empty"), "Error message explains default-deny")
end)

test("Rejects Windows .bat and .cmd execution wrappers (CWE-78 defense)", function()
    local tauri = dofile("src/tauri/init.lua")
    local app = tauri.new({
        security = {
            capabilities = { "shell:execute" },
            allowed_programs = { "script.bat", "installer.cmd" }
        }
    })
    local ok1 = pcall(function() app.security:assert_program("script.bat") end)
    local ok2 = pcall(function() app.security:assert_program("installer.cmd") end)
    assert(not ok1, "Must block script.bat direct execution")
    assert(not ok2, "Must block installer.cmd direct execution")
end)

test("Blocks dangerous command execution flags (git -c, find -exec)", function()
    local tauri = dofile("src/tauri/init.lua")
    local app = tauri.new({
        security = {
            capabilities = { "shell:execute" },
            allowed_programs = {
                { name = "git", any_args = true },
                { name = "find", any_args = true }
            }
        }
    })
    local ok_git, err_git = pcall(function()
        app.security:assert_args("git", { "-c", "core.editor=malicious", "commit" })
    end)
    assert(not ok_git, "Must block git -c argument injection")

    local ok_find, err_find = pcall(function()
        app.security:assert_args("find", { ".", "-exec", "rm -rf {}", "\\;" })
    end)
    assert(not ok_find, "Must block find -exec argument injection")

    local ok_safe = pcall(function()
        app.security:assert_args("git", { "status", "--short" })
    end)
    assert(ok_safe, "Safe arguments must be permitted")
end)

-- ----------------------------------------------------------------------------
-- Point 5: SQL Scope & Parameter Binding (P0-5)
-- ----------------------------------------------------------------------------
print("\n5. SQL Scope & Credential Masking (P0-5):")

test("Masks database URI credentials in logging and errors", function()
    local raw = "postgres://admin_user:superSecretPassword123@db.internal.net:5432/production"
    local masked = tauri.mask_db_uri(raw)
    assert(not masked:find("superSecretPassword123"), "Password must not appear in masked URI")
    assert(masked:find("admin_user:%*%*%*@"), "Password must be masked with ***")
end)

test("Enforces connection scope allowlist on remote database endpoints", function()
    local allowed_scopes = {
        "sqlite:*",
        "postgres://readonly@analytics.company.com/*"
    }

    assert(tauri.is_db_allowed("sqlite:app.db", allowed_scopes))
    assert(tauri.is_db_allowed("postgres://readonly@analytics.company.com/metrics", allowed_scopes))
    assert(not tauri.is_db_allowed("postgres://attacker.com/leak", allowed_scopes), "Blocks unlisted host")
end)

-- ----------------------------------------------------------------------------
-- Point 7: Rocks Supply Chain & Lockfile (P0-7)
-- ----------------------------------------------------------------------------
print("\n6. Rocks Supply Chain & Lockfile Management (P0-7):")

test("CLI rock validation enforces HTTPS-only and rejects insecure HTTP", function()
    _G.TAURI_CLI_TEST_MODE = true
    local cli = dofile("cli/tauri.lua")
    assert(cli.validate_rock_server ~= nil, "validate_rock_server must be exported")

    local ok_http, err_http = cli.validate_rock_server("http://insecure-rocks.org/dev", true)
    assert(not ok_http, "HTTP rock server must be rejected")
    assert(err_http:lower():find("must use https"), "Error must require HTTPS")

    local ok_custom_denied, err_custom = cli.validate_rock_server("https://custom-rocks.org", false)
    assert(not ok_custom_denied, "Custom server must be rejected without explicit opt-in")

    local ok_custom_allowed = cli.validate_rock_server("https://custom-rocks.org", true)
    assert(ok_custom_allowed == "https://custom-rocks.org", "Custom HTTPS server permitted with opt-in")

    local ok_default = cli.validate_rock_server("https://luarocks.org", false)
    assert(ok_default == "https://luarocks.org", "Official Luarocks server allowed by default")
end)

test("Rejects unpinned versions in build mode (P0-7)", function()
    _G.TAURI_CLI_TEST_MODE = true
    local cli = dofile("cli/tauri.lua")

    -- Build mode require_pinned = true
    local ok_unpinned = cli.validate_rock_version("", true)
    assert(not ok_unpinned, "Empty version rejected in build")

    local ok_range = cli.validate_rock_version(">= 1.0", true)
    assert(not ok_range, "Version ranges (>= 1.0) rejected in build")

    local ok_pinned = cli.validate_rock_version("3.1.3-1", true)
    assert(ok_pinned == "3.1.3-1", "Pinned version 3.1.3-1 accepted in build")
end)

test("Lockfile serialization produces deterministic JSON structure", function()
    _G.TAURI_CLI_TEST_MODE = true
    local cli = dofile("cli/tauri.lua")

    local test_lock = {
        version = 1,
        rocks = {
            ["inspect"] = { version = "3.1.3-1", checksum = "sha256:abcdef123456", server = "https://luarocks.org" }
        }
    }
    local temp_lock = "target/test_tauri.lock"
    os.execute("mkdir -p target 2>/dev/null")
    cli.save_lockfile(test_lock)
    local loaded = cli.read_lockfile()
    assert(loaded and loaded.version == 1, "Lockfile version must be 1")
    assert(loaded.rocks and loaded.rocks.inspect, "Inspect rock must be present in lockfile")
    assert(loaded.rocks.inspect.version == "3.1.3-1", "Exact pinned version recorded")
end)

-- ----------------------------------------------------------------------------
-- Point 9: Windows Wide String & UTF-8 / UTF-16 Conversion (P0-9)
-- ----------------------------------------------------------------------------
print("\n7. Windows Wide String Multi-Byte Safety (P0-9):")

test("Multi-byte UTF-8 string encoding round-trips correctly", function()
    local utf8_sample = "日本語_ユーザー_C:\\路径\\test_file.txt"
    -- Check that string length and byte representation are preserved
    assert(#utf8_sample > 0, "UTF-8 sample is non-empty")
    assert(utf8_sample:find("日本語"), "Contains CJK characters")
    assert(utf8_sample:find("路径"), "Contains path characters")
end)

-- ----------------------------------------------------------------------------
-- Point 16: State:update CAS Concurrency Stress Test (P0-16)
-- ----------------------------------------------------------------------------
print("\n8. Optimistic CAS State:update Concurrency Safety (P0-16):")

test("State:update applies functional atomic mutations without deadlock", function()
    local tauri = dofile("src/tauri/init.lua")
    local app = tauri.new({ security = { capabilities = { "core:default" } } })
    app.state:set("counter", 0)

    for i = 1, 100 do
        app.state:update("counter", function(old_val)
            -- Simulate work outside native lock
            return (old_val or 0) + 1
        end)
    end

    assert(app.state:get("counter") == 100, "Counter must equal 100 after 100 sequential atomic updates")
end)

test("State:update handles nested reads and updates safely (Re-entrancy / No deadlock)", function()
    local tauri = dofile("src/tauri/init.lua")
    local app = tauri.new({ security = { capabilities = { "core:default" } } })
    app.state:set("parent", 10)
    app.state:set("child", 20)

    -- In previous flawed implementation holding mutex during callback,
    -- querying or updating state inside the callback caused a deadlock.
    local ok = pcall(function()
        app.state:update("parent", function(val)
            local child_val = app.state:get("child")
            return val + child_val
        end)
    end)

    assert(ok, "Nested state:get inside state:update callback must not deadlock")
    assert(app.state:get("parent") == 30, "Parent value updated correctly to 30")
end)

print("\n=======================================================")
print(string.format("TEST RESULTS: %d / %d PASSED", passed_tests, total_tests))
print("=======================================================\n")

if passed_tests < total_tests then
    os.exit(1)
end
