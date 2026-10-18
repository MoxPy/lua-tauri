-- Tests for Step 1: Updater (§6) and Filesystem Authorization (§3)
package.path = "src/?.lua;src/?/init.lua;" .. package.path
local Tauri = require("tauri")

local function assert_error(fn, msg_match, label)
    local ok, err = pcall(fn)
    if ok then
        error(string.format("[%s] Expected error, but call SUCCEEDED", label or "test"))
    end
    if msg_match and not tostring(err):find(msg_match, 1, true) then
        error(string.format("[%s] Expected error matching '%s', got '%s'", label or "test", msg_match, tostring(err)))
    end
end

local function assert_ok(fn, label)
    local ok, err = pcall(fn)
    if not ok then
        error(string.format("[%s] Expected success, got error: %s", label or "test", tostring(err)))
    end
end

print("=== Running Step 1 Pre-Fix / Regression Checks ===")

-- 1. Updater: must fail closed and NOT be enabled
local app_core = Tauri.new({ security = { capabilities = {"core:default"} } })
print("1. Testing Updater Disabled:")
local ok_check, err_check = pcall(function() return app_core.commands["tauri:updater:check"]({}) end)
print("  - updater:check ok? " .. tostring(ok_check) .. " (result/err: " .. tostring(ok_check and err_check.version or err_check) .. ")")

local ok_inst, err_inst = pcall(function() return app_core.commands["tauri:updater:install"]({}) end)
print("  - updater:install ok? " .. tostring(ok_inst) .. " (result/err: " .. tostring(err_inst) .. ")")

-- 2. Scope empty: must DENY all access (fail closed)
print("2. Testing Empty Scope Deny-by-Default:")
local app_empty = Tauri.new({ security = { capabilities = {"fs:read-text-file"}, scope = {} } })
local ok_empty, err_empty = pcall(function()
    app_empty.security:assert_scope("/etc/passwd", app_empty.base_paths)
end)
print("  - empty scope allowed /etc/passwd? " .. tostring(ok_empty) .. " (err: " .. tostring(err_empty) .. ")")

-- 3. Scope wildcard: $DESKTOP/* must allow files in Desktop
print("3. Testing Wildcard Matching ($DESKTOP/*):")
local app_scope = Tauri.new({ security = { capabilities = {"fs:read-text-file"}, scope = {"$DESKTOP/*", "$HOME/Documents/**"} } })
local desktop_file = app_scope.base_paths.desktop .. "/notes.txt"
local ok_desk, err_desk = pcall(function()
    app_scope.security:assert_scope(desktop_file, app_scope.base_paths)
end)
print("  - $DESKTOP/* allowed desktop/notes.txt? " .. tostring(ok_desk) .. " (err: " .. tostring(err_desk) .. ")")

-- 4. Scope traversal outside allowed root
print("4. Testing Path Traversal Rejection:")
local traversal_path = app_scope.base_paths.desktop .. "/../.ssh/id_rsa"
local ok_trav, err_trav = pcall(function()
    app_scope.security:assert_scope(traversal_path, app_scope.base_paths)
end)
print("  - Traversal to ~/.ssh/id_rsa allowed? " .. tostring(ok_trav) .. " (err: " .. tostring(err_trav) .. ")")

-- 5. Scope NUL byte injection
print("5. Testing NUL Byte Rejection:")
local nul_path = app_scope.base_paths.desktop .. "/notes.txt\0/../../etc/passwd"
local ok_nul, err_nul = pcall(function()
    app_scope.security:assert_scope(nul_path, app_scope.base_paths)
end)
print("  - NUL byte path allowed? " .. tostring(ok_nul) .. " (err: " .. tostring(err_nul) .. ")")

-- 6. tauri:fs:remove capability
print("6. Testing tauri:fs:remove capability:")
local app_writeonly = Tauri.new({ security = { capabilities = {"fs:write-text-file"}, scope = {"$DESKTOP/*"} } })
local ok_remove, err_remove = pcall(function()
    return app_writeonly.commands["tauri:fs:remove"]({ path = desktop_file })
end)
print("  - fs:remove allowed with only fs:write-text-file? " .. tostring(ok_remove) .. " (err: " .. tostring(err_remove) .. ")")
