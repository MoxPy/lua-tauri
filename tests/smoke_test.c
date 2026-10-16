#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "taurilua.h"

int main() {
#ifdef _WIN32
    _putenv("TAURILUA_WORKERS=0");
#else
    setenv("TAURILUA_WORKERS", "0", 0);
#endif
    printf("[Smoke Test] Starting Tauri-Lua headless smoke test...\n");
    taurilua_config_t cfg;
    taurilua_config_init(&cfg);
    cfg.title = "Smoke Test Window";
    cfg.width = 600;
    cfg.height = 400;

    taurilua_app_t *app = taurilua_create(&cfg);
    if (!app) {
        fprintf(stderr, "[Smoke Test] FAILED: could not create taurilua_app_t\n");
        return 1;
    }

    // Register a test command and invoke it
    const char *test_lua =
        "local Tauri = require('tauri')\n"
        "assert(Tauri ~= nil, 'Tauri module failed to load')\n"
        "print('[Smoke Test] Tauri core module loaded successfully')\n"
        "local app = Tauri.new({ security = { capabilities = {'core:default', 'fs:read-text-file'} } })\n"
        "app:register('smoke_ping', function(payload) return { pong = true, received = payload.msg } end)\n"
        "local res = app.commands['smoke_ping']({ msg = 'hello-from-ci' })\n"
        "assert(res and res.pong == true and res.received == 'hello-from-ci', 'invoke test failed')\n"
        "print('[Smoke Test] Test invoke smoke_ping verified successfully!')\n";

    if (taurilua_eval_lua(app, test_lua) != 0) {
        fprintf(stderr, "[Smoke Test] FAILED: Lua evaluation failed\n");
        taurilua_destroy(app);
        return 1;
    }

    printf("[Smoke Test] Terminating app instance...\n");
    taurilua_terminate(app);
    taurilua_destroy(app);
    printf("[Smoke Test] SUCCESS: All smoke checks passed!\n");
    return 0;
}
