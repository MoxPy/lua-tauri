local tauri = dofile("src/tauri/init.lua")

print("Running Sidecar Unit Tests...")

-- Test 1: Sidecar resolution with non-existent binary returns nil
local resolved = tauri.Sidecar.resolve_path("non_existent_binary_12345", "TestApp")
assert(resolved == nil, "Non existent binary must return nil")
print("  - Non-existent binary returns nil: PASS")

-- Test 2: Sidecar resolution with existent binary path
local test_bin = "/bin/sh"
local resolved_sh = tauri.Sidecar.resolve_path(test_bin, "TestApp")
assert(resolved_sh == test_bin, "Direct binary path must resolve correctly")
print("  - Direct binary path resolves: PASS")

-- Test 3: app:spawn_sidecar lifecycle and IPC registration
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

-- Test 4: IPC command auto-registration
assert(app.commands["test:get_sidecar"] ~= nil, "IPC command must be registered")
local tbl = app.commands["test:get_sidecar"]()
assert(tbl.port == 9099, "IPC table port must match")
assert(tbl.running == true, "IPC table running must be true")
print("  - IPC command auto-registration: PASS")

-- Test 5: Clean shutdown
sc:kill()
-- Small wait to allow process termination
os.execute("sleep 0.1")
assert(sc:is_running() == false, "Sidecar must not be running after kill")
print("  - Sidecar kill and process termination: PASS")

print("ALL SIDECAR TESTS PASSED!")
