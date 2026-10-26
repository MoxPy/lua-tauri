_G.__TAURI_DEV__ = true
local tauri = dofile("src/tauri/init.lua")

print("Running Sidecar Unit Tests...")

local resolved = tauri.Sidecar.resolve_path("non_existent_binary_12345", "TestApp")
assert(resolved == nil, "Non existent binary must return nil")
print("  - Non-existent binary returns nil: PASS")

local test_bin = "/bin/sh"
local resolved_sh = tauri.Sidecar.resolve_path(test_bin, "TestApp")
assert(resolved_sh == test_bin, "Direct binary path must resolve correctly")
print("  - Direct binary path resolves: PASS")

local env_none = tauri.Sidecar.find_env_file(nil, "/bin/sh", "NonExistentApp123XYZ")
assert(env_none == nil or type(env_none) == "string", "find_env_file returns nil or string")

local tmp_env = "/tmp/test_sidecar_" .. tostring(os.time()) .. ".env"
local f_env = io.open(tmp_env, "w")
if f_env then
    f_env:write("FOO=BAR\n")
    f_env:close()
    local found = tauri.Sidecar.find_env_file(tmp_env, "/bin/sh", "TestApp")
    assert(found == tmp_env, "Explicit env file must be found")
    local found_legacy = tauri.Sidecar.find_env_file("/bin/sh", "TestApp", tmp_env)
    assert(found_legacy == tmp_env, "Legacy signature finds explicit env file")
    os.remove(tmp_env)
end
print("  - Sidecar.find_env_file candidate resolution: PASS")

local app = tauri.init({
    productName = "TestApp",
    version = "1.0.0"
})

local sc = app:spawn_sidecar("/bin/sh", {
    args = { "-c", "sleep 10" },
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
os.execute("sleep 0.1")
assert(sc:is_running() == false, "Sidecar must not be running after kill")
print("  - Sidecar kill and process termination: PASS")

if sc.log_file and sc.log_file ~= "/dev/null" and sc.log_file ~= "NUL" then
    os.remove(sc.log_file)
end
os.execute("rm -rf logs 2>/dev/null || true")

print("ALL SIDECAR TESTS PASSED!")
