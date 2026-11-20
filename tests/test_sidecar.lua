_G.__TAURI_DEV__ = true
local ffi = require("ffi")
local tauri = dofile("src/tauri/init.lua")

print("Running Sidecar Unit Tests...")

local resolved = tauri.Sidecar.resolve_path("non_existent_binary_12345", "TestApp")
assert(resolved == nil, "Non existent binary must return nil")
assert(tauri.Sidecar.resolve_path("../bin/sh", "TestApp") == nil, "Traversal in sidecar name must return nil")
print("  - Non-existent & traversal sidecar names return nil: PASS")

local is_win = ffi.os == "Windows"
local test_bin = is_win and "C:\\Windows\\System32\\ping.exe" or "/bin/sh"
local test_args = is_win and { "-n", "10", "127.0.0.1" } or { "-c", "sleep 10" }

local resolved_bin = tauri.Sidecar.resolve_path(test_bin, "TestApp")
assert(resolved_bin == test_bin, "Direct binary path must resolve correctly")
print("  - Direct binary path resolves: PASS")

local env_none = tauri.Sidecar.find_env_file(nil, test_bin, "NonExistentApp123XYZ")
assert(env_none == nil or type(env_none) == "string", "find_env_file returns nil or string")

local tmp_env = (is_win and (os.getenv("TEMP") or ".") .. "\\test_sidecar_" or "/tmp/test_sidecar_") .. tostring(os.time()) .. ".env"
local f_env = io.open(tmp_env, "wb")
if f_env then
    f_env:write("FOO=BAR\n")
    f_env:close()
    local found = tauri.Sidecar.find_env_file(tmp_env, test_bin, "TestApp")
    assert(found == tmp_env, "Explicit env file must be found")
    local found_legacy = tauri.Sidecar.find_env_file(test_bin, "TestApp", tmp_env)
    assert(found_legacy == tmp_env, "Legacy signature finds explicit env file")
    os.remove(tmp_env)
end
print("  - Sidecar.find_env_file candidate resolution: PASS")

local app = tauri.init({
    productName = "TestApp",
    version = "1.0.0"
})

local sc = app:spawn_sidecar(test_bin, {
    args = test_args,
    port = 9099,
    ipc_command = "test:get_sidecar"
})

assert(sc ~= nil, "Sidecar instance must not be nil")
assert(sc.port == 9099, "Port must match 9099")
assert(sc.base_url == "http://127.0.0.1:9099", "Base URL must be http://127.0.0.1:9099")
assert(sc.pid ~= nil and sc.pid > 0, "PID must be positive integer")
assert(sc:is_running() == true, "Sidecar must be running")
print("  - app:spawn_sidecar spawns process and tracks PID: PASS")

assert(app.commands["test:get_sidecar"] ~= nil, "IPC command must be registered")
local tbl = app.commands["test:get_sidecar"]()
assert(tbl.port == 9099, "IPC table port must match")
assert(tbl.running == true, "IPC table running must be true")
print("  - IPC command auto-registration: PASS")

sc:kill()
tauri.sleep(150)
assert(sc:is_running() == false, "Sidecar must not be running after kill")
print("  - Sidecar kill and process termination: PASS")

if sc.log_file and sc.log_file ~= "/dev/null" and sc.log_file ~= "NUL" then
    os.remove(sc.log_file)
end

print("ALL SIDECAR TESTS PASSED!")
