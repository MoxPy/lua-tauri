-- ===========================================================================
-- tests/test_security_set1.lua
-- Comprehensive Regression Test Suite for lua-tauri Remediation (Stage 1)
-- Exercises real shipped code in src/tauri/init.lua
-- ===========================================================================

package.path = "src/?.lua;src/?/init.lua;" .. package.path
local tauri = require("tauri")

local passed_count = 0
local total_count = 0

local function check(name, condition, detail)
    total_count = total_count + 1
    if condition then
        passed_count = passed_count + 1
        print(string.format("  [PASS] %s", name))
    else
        print(string.format("  [FAIL] %s: %s", name, detail or "assertion failed"))
        error("Test failure: " .. name)
    end
end

print("=======================================================")
print("RUNNING SET 1 SECURITY REGRESSION TESTS (SHIPPED CODE)")
print("=======================================================")

-- ---------------------------------------------------------------------------
-- 1. Filesystem Scope & Path Matching (§3)
-- ---------------------------------------------------------------------------
print("\n1. Testing Filesystem Authorization & Scope Semantics (§3):")
do
    -- (a) Empty scopes list MUST deny by default
    local app_empty_scopes = tauri.new({
        security = {
            capabilities = { "fs:read-text-file", "fs:write-text-file", "fs:remove" },
            scope = {} -- EMPTY SCOPES
        }
    })

    local ok1, err1 = pcall(function()
        app_empty_scopes.security:assert_scope("/etc/passwd", app_empty_scopes.base_paths)
    end)
    check("Empty scope list denies /etc/passwd (deny-by-default)", not ok1 and err1:find("deny%-by%-default") ~= nil)

    local ok2, err2 = pcall(function()
        app_empty_scopes.security:assert_scope("relative/path.txt", app_empty_scopes.base_paths)
    end)
    check("Empty scope list denies relative paths", not ok2 and err2:find("deny%-by%-default") ~= nil)

    -- (b) Documented wildcards (* = single segment, ** = recursive)
    local app_scoped = tauri.new({
        security = {
            capabilities = { "fs:default", "fs:write-text-file", "fs:remove" },
            scope = {
                "$DESKTOP/*",           -- direct children only
                "$HOME/Documents/**",   -- recursive subdirectories
                "$APPDATA/**"
            }
        }
    })

    local desktop = app_scoped.base_paths.desktop
    local home = app_scoped.base_paths.home

    -- Direct child under $DESKTOP/* allowed
    local ok3, err3 = pcall(function()
        app_scoped.security:assert_scope(desktop .. "/notes.txt", app_scoped.base_paths)
    end)
    check("$DESKTOP/* allows direct file (desktop/notes.txt)", ok3, err3)

    -- Nested child under $DESKTOP/* denied (* does not cross /)
    local ok4, _ = pcall(function()
        app_scoped.security:assert_scope(desktop .. "/sub/notes.txt", app_scoped.base_paths)
    end)
    check("$DESKTOP/* denies nested file (desktop/sub/notes.txt)", not ok4)

    -- Recursive wildcard $HOME/Documents/** allows nested files
    local ok5, err5 = pcall(function()
        app_scoped.security:assert_scope(home .. "/Documents/sub/deep/file.txt", app_scoped.base_paths)
    end)
    check("$HOME/Documents/** allows deep nested file", ok5, err5)

    -- N1 Regression: Sibling lookalike directories denied
    local ok6a, _ = pcall(function()
        app_scoped.security:assert_scope(home .. "/Documentsevil/x", app_scoped.base_paths)
    end)
    check("N1: Sibling lookalike directory (Documentsevil/x) denied", not ok6a)

    local ok6b, _ = pcall(function()
        app_scoped.security:assert_scope(home .. "/Documents-secret/x", app_scoped.base_paths)
    end)
    check("N1: Sibling lookalike directory (Documents-secret/x) denied", not ok6b)

    local ok6c, _ = pcall(function()
        app_scoped.security:assert_scope(home .. "/DocumentsX", app_scoped.base_paths)
    end)
    check("N1: Sibling lookalike directory (DocumentsX) denied", not ok6c)

    -- N1: Legitimate paths under $HOME/Documents/** allowed
    local ok6d, _ = pcall(function()
        app_scoped.security:assert_scope(home .. "/Documents", app_scoped.base_paths)
    end)
    check("N1: Base directory itself ($HOME/Documents) allowed", ok6d)

    local ok6e, _ = pcall(function()
        app_scoped.security:assert_scope(home .. "/Documents/a", app_scoped.base_paths)
    end)
    check("N1: Direct child under recursive scope allowed", ok6e)

    local ok6f, _ = pcall(function()
        app_scoped.security:assert_scope(home .. "/Documents/a/b/c", app_scoped.base_paths)
    end)
    check("N1: Nested children under recursive scope allowed", ok6f)

    -- Traversal outside root denied
    local ok7, _ = pcall(function()
        app_scoped.security:assert_scope(desktop .. "/../.ssh/id_rsa", app_scoped.base_paths)
    end)
    check("Directory traversal (../.ssh/id_rsa) denied", not ok7)

    -- Traversal with %2e denied (R6)
    local ok8, _ = pcall(function()
        app_scoped.security:assert_scope(desktop .. "/%2e%2e/.ssh/id_rsa", app_scoped.base_paths)
    end)
    check("Percent-encoded traversal (%2e%2e) denied", not ok8)

    -- NUL byte rejection
    local ok9, err9 = pcall(function()
        app_scoped.security:assert_scope(desktop .. "/file.txt\0/../../etc/passwd", app_scoped.base_paths)
    end)
    check("NUL byte in path rejected", not ok9 and err9:find("NUL") ~= nil)

    local ffi_chk = require("ffi")
    if ffi_chk.os ~= "Windows" then
        local cwd = io.popen("pwd"):read("*l") or "."
        local temp_root = cwd .. "/target/tauri_test_symlink_" .. tostring(os.time())
        os.execute("mkdir -p " .. temp_root .. "/scope " .. temp_root .. "/secret")
        local f = io.open(temp_root .. "/secret/topsecret.txt", "w")
        if f then
            f:write("TOPSECRET_CONTENT")
            f:close()
        end
        os.execute("ln -s " .. temp_root .. "/secret/topsecret.txt " .. temp_root .. "/scope/symlink_out.txt 2>/dev/null")

        local app_symlink = tauri.new({
            security = {
                capabilities = { "fs:read-text-file", "fs:write-text-file", "fs:remove" },
                scope = { temp_root .. "/scope/**" }
            }
        })

        local ok_sym, err_sym = pcall(function()
            app_symlink.security:assert_scope(temp_root .. "/scope/symlink_out.txt", app_symlink.base_paths)
        end)
        check("N2: Symlink escaping allowed scope root is denied", not ok_sym)

        os.execute("rm -rf " .. temp_root)
    end

    -- (c) fs:remove requires its own capability
    local app_write_only = tauri.new({
        security = {
            capabilities = { "fs:write-text-file" },
            scope = { "$DESKTOP/**" }
        }
    })
    local ok10, err10 = pcall(function()
        app_write_only.commands["tauri:fs:remove"]({ path = desktop .. "/file.txt" })
    end)
    check("tauri:fs:remove requires explicit fs:remove capability", not ok10 and err10:find("fs:remove") ~= nil)
end

-- ---------------------------------------------------------------------------
-- 2. Capability Semantics & Presets (§5)
-- ---------------------------------------------------------------------------
print("\n2. Testing Capability Semantics & Documented Presets (§5):")
do
    -- fs:default only grants read/dir/stat, NOT write or remove
    local app_fs_default = tauri.new({
        security = {
            capabilities = { "fs:default" },
            scope = { "$DESKTOP/**" }
        }
    })
    local ok_read = pcall(function() app_fs_default.security:assert_capability("fs:read-text-file") end)
    local ok_dir = pcall(function() app_fs_default.security:assert_capability("fs:read-dir") end)
    local ok_write = pcall(function() app_fs_default.security:assert_capability("fs:write-text-file") end)
    local ok_remove = pcall(function() app_fs_default.security:assert_capability("fs:remove") end)

    check("fs:default grants fs:read-text-file", ok_read)
    check("fs:default grants fs:read-dir", ok_dir)
    check("fs:default DOES NOT grant fs:write-text-file", not ok_write)
    check("fs:default DOES NOT grant fs:remove", not ok_remove)

    -- core:default does NOT grant updater or external window creation
    local app_core_default = tauri.new({
        security = {
            capabilities = { "core:default" }
        }
    })
    local ok_win = pcall(function() app_core_default.security:assert_capability("window:create") end)
    local ok_ext = pcall(function() app_core_default.security:assert_capability("window:create:external") end)
    local ok_upd = pcall(function() app_core_default.security:assert_capability("updater:check") end)

    check("core:default grants core window commands", ok_win)
    check("core:default DOES NOT grant window:create:external", not ok_ext)
    check("core:default DOES NOT grant updater capabilities", not ok_upd)
end

-- ---------------------------------------------------------------------------
-- 3. Unauthenticated Updater (§6)
-- ---------------------------------------------------------------------------
print("\n3. Testing Unauthenticated Updater Disabled (§6):")
do
    local app = tauri.new({
        security = { capabilities = { "core:default" } }
    })
    local ok_chk, err_chk = pcall(function() app.commands["tauri:updater:check"]({}) end)
    check("tauri:updater:check fails closed", not ok_chk and err_chk:find("disabilitato") ~= nil)

    local ok_inst, err_inst = pcall(function() app.commands["tauri:updater:install"]({}) end)
    check("tauri:updater:install fails closed", not ok_inst and err_inst:find("disabilitato") ~= nil)
end

-- ---------------------------------------------------------------------------
-- 4. IPC Authorization Completeness (§8)
-- ---------------------------------------------------------------------------
print("\n4. Testing IPC Authorization Completeness (§8):")
do
    -- When capabilities list is empty, ALL builtins must be rejected
    local app_empty_caps = tauri.new({
        security = {
            capabilities = {} -- EMPTY CAPABILITIES
        }
    })

    local denied_cmds = {
        { "tauri:store:get", { key = "test" } },
        { "tauri:store:set", { key = "test", value = 1 } },
        { "tauri:store:has", { key = "test" } },
        { "tauri:store:delete", { key = "test" } },
        { "tauri:store:clear", {} },
        { "tauri:state:get", { key = "test" } },
        { "tauri:state:set", { key = "test", value = 1 } },
        { "tauri:event:emit", { event = "ev", payload = {} } },
        { "tauri:theme:get", {} },
        { "tauri:path:app_data_dir", {} },
        { "tauri:path:home_dir", {} },
        { "tauri:path:desktop_dir", {} },
        { "tauri:window:create", {} },
        { "tauri:window:close", {} },
        { "tauri:dialog:open", {} },
        { "tauri:dialog:message", {} },
        { "tauri:clipboard:read_text", {} },
        { "tauri:clipboard:write_text", { text = "hi" } },
        { "tauri:notification:send", { title = "t" } },
    }

    for _, c in ipairs(denied_cmds) do
        local cmd = c[1]
        local payload = c[2]
        local ok, err = pcall(function()
            app_empty_caps.commands[cmd](payload)
        end)
        check(string.format("Empty capabilities denies '%s'", cmd), not ok and err:find("Capability non autorizzata") ~= nil)
    end

    -- N3: Window Create URL gating: external / lookalike URLs require window:create:external
    local app_core = tauri.new({
        security = { capabilities = { "core:default" } }
    })
    local ok_ext, err_ext = pcall(function()
        app_core.commands["tauri:window:create"]({ url = "https://attacker.example/phish.html" })
    end)
    check("N3: tauri:window:create denies external URL under core:default", not ok_ext and err_ext:find("window:create:external") ~= nil)

    local ok_n3_1, err_n3_1 = pcall(function()
        app_core.commands["tauri:window:create"]({ url = "https://tauri.localhost.evil.com/x" })
    end)
    check("N3: tauri:window:create denies https://tauri.localhost.evil.com/x", not ok_n3_1 and err_n3_1:find("window:create:external") ~= nil)

    local ok_n3_2, err_n3_2 = pcall(function()
        app_core.commands["tauri:window:create"]({ url = "https://tauri.localhost@evil.com/" })
    end)
    check("N3: tauri:window:create denies https://tauri.localhost@evil.com/", not ok_n3_2 and err_n3_2:find("window:create:external") ~= nil)

    local ok_n3_3, err_n3_3 = pcall(function()
        app_core.commands["tauri:window:create"]({ url = "tauri://evil/x" })
    end)
    check("N3: tauri:window:create denies tauri://evil/x", not ok_n3_3 and err_n3_3:find("window:create:external") ~= nil)

    -- R1: Event dispatch and __TAURI_DISPATCH_EMIT__
    local received_event = nil
    app_core:listen("app://sync-event", function(payload)
        received_event = payload
    end)
    _G.__TAURI_DISPATCH_EMIT__("app://sync-event", "{\"data\":\"verified\", \"id\":42}")
    check("R1: __TAURI_DISPATCH_EMIT__ dispatches safely to app listeners", received_event and received_event.data == "verified" and received_event.id == 42)
end

-- ---------------------------------------------------------------------------
-- 5. Strict RFC 8259 JSON Parser & Encoder (§10)
-- ---------------------------------------------------------------------------
print("\n5. Testing Strict RFC 8259 JSON Parser & Encoder (§10):")
do
    local json = tauri.new().json

    -- (a) Malformed inputs must return nil, err quickly (no hang or infinite loop)
    local malformed_inputs = {
        "[1, 2",
        "{\"a\": 1",
        "{\"a\":",
        "{\"a\"",
        "[1, 2,]",
        "{\"a\": 1,}",
        "{,}",
        "[,]",
        "\"unterminated string",
        "true false",
        "{\"a\": 1} 123",
        "123abc",
        "0123",
        "+123",
        ".5",
        "1.",
        "1e",
        "",
        "   "
    }

    for _, inp in ipairs(malformed_inputs) do
        local res, err = json.decode(inp)
        check(string.format("Malformed JSON '%s' returns nil, err", inp), res == nil and type(err) == "string")
    end

    -- (b) Sequential escape unescaping: "\\n" must decode to '\' and 'n', NOT newline!
    local dec_slash_n = json.decode("\"a\\\\nb\"")
    check("Sequential escape: '\\\\n' does not become newline", dec_slash_n == "a\\nb")

    -- (c) Unicode escapes and surrogate pairs
    local dec_unicode = json.decode("\"\\u0041\\u0042\"")
    check("Unicode escape: '\\u0041\\u0042' decodes to 'AB'", dec_unicode == "AB")

    local dec_surrogate = json.decode("\"\\uD83D\\uDE00\"")
    check("Surrogate pair decodes to smiley emoji", dec_surrogate == "\240\159\152\128")

    -- (d) Rejection of unescaped control characters in strings
    local dec_ctrl, err_ctrl = json.decode("\"\n\"")
    check("Unescaped newline in JSON string is rejected", dec_ctrl == nil and err_ctrl ~= nil)

    local dec_nul, err_nul = json.decode("\"\0\"")
    check("Unescaped NUL in JSON string is rejected", dec_nul == nil and err_nul ~= nil)

    -- (e) Encoder NaN / Infinity rejection
    local ok_nan, _ = pcall(function() json.encode(0/0) end)
    check("Encoder rejects NaN", not ok_nan)

    local ok_inf, _ = pcall(function() json.encode(1/0) end)
    check("Encoder rejects Infinity", not ok_inf)

    -- (f) Encoder cycle detection
    local cycle_t = {}
    cycle_t.self = cycle_t
    local ok_cycle, err_cycle = pcall(function() json.encode(cycle_t) end)
    check("Encoder detects circular reference", not ok_cycle and err_cycle:find("circular") ~= nil)
end

-- ---------------------------------------------------------------------------
-- 6. Shell Plugin Hardening (§11)
-- ---------------------------------------------------------------------------
print("\n6. Testing Shell Plugin Hardening (§11):")
do
    -- (a) Missing or empty allowed_programs denies execution
    local app_empty_progs = tauri.new({
        security = {
            capabilities = { "shell:execute" },
            allowed_programs = {}
        }
    })
    local ok_prog, err_prog = pcall(function()
        app_empty_progs.security:assert_program("ls")
    end)
    check("Empty allowed_programs denies execution", not ok_prog and err_prog:find("denied") ~= nil)

    -- (b) .bat and .cmd execution blocked (CWE-78 defense)
    local app_bat = tauri.new({
        security = {
            capabilities = { "shell:execute" },
            allowed_programs = { "script.bat", "runner.cmd" }
        }
    })
    local ok_bat, err_bat = pcall(function()
        app_bat.security:assert_program("script.bat")
    end)
    check("Execution of .bat file is blocked", not ok_bat and err_bat:find("CWE%-78") ~= nil)

    local ok_cmd, err_cmd = pcall(function()
        app_bat.security:assert_program("runner.cmd")
    end)
    check("Execution of .cmd file is blocked", not ok_cmd and err_cmd:find("CWE%-78") ~= nil)

    -- (c) Strict per-program argument allowlist
    local app_strict_args = tauri.new({
        security = {
            capabilities = { "shell:execute" },
            allowed_programs = {
                { name = "git", args = { "status", "version" } }
            }
        }
    })
    local ok_arg_pass, _ = pcall(function()
        app_strict_args.security:assert_args("git", { "status" })
    end)
    check("Whitelisted argument ('status') permitted", ok_arg_pass)

    local ok_arg_fail, _ = pcall(function()
        app_strict_args.security:assert_args("git", { "clone", "http://evil.com" })
    end)
    check("Unlisted argument ('clone') rejected", not ok_arg_fail)

    -- (d) R4: String entry defaults to denying non-empty arguments (no arg allowlist)
    local app_open_git = tauri.new({
        security = {
            capabilities = { "shell:execute" },
            allowed_programs = { "git" } -- string entry: no arg allowlist
        }
    })
    local ok_no_arg = pcall(function()
        app_open_git.security:assert_args("git", {})
    end)
    check("R4: String entry allows command with empty arguments", ok_no_arg)

    local ok_str_args, err_str_args = pcall(function()
        app_open_git.security:assert_args("git", { "status" })
    end)
    check("R4: String entry denies arguments without explicit allowlist/opt-in", not ok_str_args and err_str_args:find("no arg allowlist") ~= nil)

    -- (e) Dangerous argument blocking and explicit any_args opt-in (R4)
    local app_any_git = tauri.new({
        security = {
            capabilities = { "shell:execute" },
            allowed_programs = {
                { name = "git", any_args = true }
            }
        }
    })
    local ok_safe_any = pcall(function()
        app_any_git.security:assert_args("git", { "status", "--short" })
    end)
    check("R4: Explicit any_args = true permits safe non-empty arguments", ok_safe_any)

    local ok_dang, err_dang = pcall(function()
        app_any_git.security:assert_args("git", { "-c", "core.fsmonitor=calc.exe" })
    end)
    check("Dangerous git argument ('-c') is blocked even with any_args = true", not ok_dang and err_dang:find("dangerous") ~= nil)

    local ok_any_prog = pcall(function()
        app_any_git.security:assert_program("git")
    end)
    check("Phase 1: assert_program succeeds when any_args = true", ok_any_prog)

    local ok_nul_arg = pcall(function()
        app_any_git.security:assert_args("git", { "status\0--exec" })
    end)
    check("Phase 1: NUL byte in shell args rejected", not ok_nul_arg)

    local ok_git_C = pcall(function()
        app_any_git.security:assert_args("git", { "-C", "/tmp", "status" })
    end)
    check("Phase 1: git -C blocked", not ok_git_C)
end

print("\n7. Phase 1 Hardening Regression Tests:")
do
    local ffi_p1 = require("ffi")
    local app_p1_caps = tauri.new({
        security = {
            capabilities = { "fs:read-text-file", "fs:write-text-file", "fs:read-dir", "shell:default", "window:*" },
            scope = { "$APPDATA/**" }
        }
    })

    local ok_empty_appdata = pcall(function()
        app_p1_caps.security:assert_scope("/etc/passwd", { home = "/tmp", appData = "", desktop = "/tmp" })
    end)
    check("Phase 1: Empty APPDATA does not widen $APPDATA/** to root", not ok_empty_appdata)

    local ok_shell_def = pcall(function()
        app_p1_caps.security:assert_capability("shell:execute")
    end)
    check("Phase 1: shell:default does not grant shell:execute", not ok_shell_def)

    local ok_win_star = pcall(function()
        app_p1_caps.security:assert_capability("window:create:external")
    end)
    check("Phase 1: window:* grants window:create:external", ok_win_star)

    if ffi_p1.os ~= "Windows" then
        local cwd = io.popen("pwd"):read("*l") or "."
        local temp_root = cwd .. "/target/tauri_phase1_" .. tostring(os.time())
        os.execute("mkdir -p " .. temp_root .. "/scope/sub " .. temp_root .. "/outside")
        local f_in = io.open(temp_root .. "/scope/hello.txt", "wb")
        if f_in then f_in:write("hi") f_in:close() end
        os.execute("ln -s " .. temp_root .. "/outside/new_secret.txt " .. temp_root .. "/scope/dangling_out.txt 2>/dev/null")

        local app_p1 = tauri.new({
            security = {
                capabilities = { "fs:read-text-file", "fs:write-text-file", "fs:read-dir" },
                scope = { temp_root .. "/scope/**" }
            }
        })

        local ok_dangling = pcall(function()
            app_p1.security:assert_scope(temp_root .. "/scope/dangling_out.txt", app_p1.base_paths)
        end)
        check("Phase 1: Dangling symlink escaping scope is denied", not ok_dangling)

        local ok_dir, dir_entries = pcall(function()
            return app_p1.commands["tauri:fs:read_dir"]({ path = temp_root .. "/scope" })
        end)
        if ok_dir and type(dir_entries) == "table" then
            local found_hello = false
            local found_sub = false
            for _, e in ipairs(dir_entries) do
                if e.name == "hello.txt" and e.isDirectory == false then found_hello = true end
                if e.name == "sub" and e.isDirectory == true then found_sub = true end
            end
            check("Phase 1: tauri:fs:read_dir returns accurate native entries", found_hello and found_sub)
        end

        os.execute("rm -rf " .. temp_root)
    end

    check("Phase 1: SSRF blocks unbracketed IPv6 fe80::1", tauri.is_ssrf_forbidden("fe80::1"))
    check("Phase 1: SSRF blocks unbracketed IPv6 fd00:ec2::254", tauri.is_ssrf_forbidden("fd00:ec2::254"))
    check("Phase 1: SSRF blocks [0::1]", tauri.is_ssrf_forbidden("[0::1]"))
    check("Phase 1: SSRF blocks [0:0:0:0:0:ffff:7f00:1]", tauri.is_ssrf_forbidden("[0:0:0:0:0:ffff:7f00:1]"))
    check("Phase 1: SSRF blocks [64:ff9b::127.0.0.1]", tauri.is_ssrf_forbidden("[64:ff9b::127.0.0.1]"))
    check("Phase 1: SSRF blocks abbreviated IPv4 127.1", tauri.is_ssrf_forbidden("127.1"))
    check("Phase 1: SSRF blocks hex IPv4 0x7f.0.0.1", tauri.is_ssrf_forbidden("0x7f.0.0.1"))
    check("Phase 1: SSRF blocks multicast 224.0.0.1", tauri.is_ssrf_forbidden("224.0.0.1"))
    check("Phase 1: SSRF blocks metadata.goog", tauri.is_ssrf_forbidden("metadata.goog"))
    check("Phase 1: SSRF allows public IPv6 2606:4700:4700::1111", not tauri.is_ssrf_forbidden("2606:4700:4700::1111"))

    check("Phase 1: is_db_allowed blocks sqlite:file:/etc/passwd", not tauri.is_db_allowed("sqlite:file:/etc/passwd", { "sqlite:*" }))
    check("Phase 1: is_db_allowed blocks @ host confusion", not tauri.is_db_allowed("postgres://readonly@analytics.company.com@evil.com/db", { "postgres://readonly@analytics.company.com/*" }))
    check("Phase 1: mask_db_uri masks password containing @", tauri.mask_db_uri("postgres://admin:p@ss@db.local:5432/app") == "postgres://admin:***@db.local:5432/app")

    local deep_tbl = {}
    local cur = deep_tbl
    for _ = 1, 160 do
        cur.next = {}
        cur = cur.next
    end
    local ok_deep = pcall(function() tauri.json.encode(deep_tbl) end)
    check("Phase 1: json.encode rejects deep recursion (>128)", not ok_deep)
end

print("\n=======================================================")
print(string.format("SET 1 REGRESSION RESULTS: %d / %d PASSED", passed_count, total_count))
print("=======================================================\n")
