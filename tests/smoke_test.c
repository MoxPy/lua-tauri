#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "taurilua.h"

static void host_echo_handler(const char *payload_json, const char *window_label, char *out_json, size_t out_capacity, int *out_is_error, void *user_data) {
    const char *prefix = (const char *)user_data;
    if (!out_json || out_capacity == 0) return;
    *out_is_error = 0;
    snprintf(out_json, out_capacity, "{\"prefix\":\"%s\",\"label\":\"%s\",\"raw\":%s}",
             prefix ? prefix : "",
             window_label ? window_label : "",
             (payload_json && payload_json[0] != '\0') ? payload_json : "{}");
}

int main() {
#ifdef _WIN32
    _putenv("TAURILUA_WORKERS=2");
#else
    setenv("TAURILUA_WORKERS", "2", 1);
#endif
    assert(TAURILUA_VERSION_MAJOR == 1);

    printf("[Smoke Test] Starting Tauri-Lua Phase 3 embedding & concurrency test...\n");
    taurilua_config_t cfg;
    taurilua_config_init(&cfg);
    cfg.app_name = "SmokeHostApp";
    cfg.title = "Smoke Test Window";
    cfg.width = 640;
    cfg.height = 480;
    cfg.resizable = 0;

    taurilua_app_t *app = taurilua_create(&cfg);
    if (!app) {
        fprintf(stderr, "[Smoke Test] FAILED: could not create taurilua_app_t\n");
        return 1;
    }

    taurilua_set_startup_payload(app, "{\"args\":[\"smoke\",\"--flag\"]}");
    assert(taurilua_register_command(app, "host_echo", host_echo_handler, (void *)"c-host") == 0);

    const char *test_lua =
        "local Tauri = require('tauri')\n"
        "assert(Tauri ~= nil, 'Tauri module failed to load')\n"
        "assert(__TAURI_HOST_OVERRIDES__ and __TAURI_HOST_OVERRIDES__.title == 'Smoke Test Window', 'host title override missing')\n"
        "assert(__TAURI_HOST_OVERRIDES__.width == 640 and __TAURI_HOST_OVERRIDES__.height == 480, 'host size override missing')\n"
        "assert(__TAURI_HOST_OVERRIDES__.resizable == false, 'host resizable override missing')\n"
        "assert(__TAURI_STARTUP_PAYLOAD__:find('--flag', 1, true), 'startup payload missing')\n"
        "local found, is_err, out_json = __TAURI_INVOKE_HOST_CMD__('host_echo', '{\"n\":7}', 'main')\n"
        "assert(found == true and is_err == false, 'host command invocation failed')\n"
        "local dec = Tauri.json.decode(out_json)\n"
        "assert(dec and dec.prefix == 'c-host' and dec.raw and dec.raw.n == 7, 'host command payload mismatch')\n"
        "local app = Tauri.new({ security = { capabilities = {'core:default', 'fs:read-text-file'} } })\n"
        "app:register('smoke_ping', function(payload) return { pong = true, received = payload.msg } end)\n"
        "local res = app.commands['smoke_ping']({ msg = 'hello-from-ci' })\n"
        "assert(res and res.pong == true and res.received == 'hello-from-ci', 'invoke test failed')\n"
        "app.state:set('bool_val', true)\n"
        "app.state:set('str_true', 'true')\n"
        "assert(type(app.state:get('bool_val')) == 'boolean' and app.state:get('bool_val') == true, 'boolean state corrupted')\n"
        "assert(type(app.state:get('str_true')) == 'string' and app.state:get('str_true') == 'true', 'string true coerced to boolean')\n"
        "local ok_nan = pcall(function() app.state:update('bad', function() return { x = 0/0 } end) end)\n"
        "assert(not ok_nan, 'NaN update must fail')\n"
        "app.state:set('after_nan', 42)\n"
        "assert(app.state:get('after_nan') == 42, 'mutex deadlocked after State:update error')\n";

    if (taurilua_eval_lua(app, test_lua) != 0) {
        fprintf(stderr, "[Smoke Test] FAILED: Lua evaluation failed: %s\n", taurilua_last_error(app));
        taurilua_destroy(app);
        return 1;
    }

    assert(taurilua_eval_lua(app, "error('expected_host_err')") != 0);
    assert(strstr(taurilua_last_error(app), "expected_host_err") != NULL);

    taurilua_terminate(app);
    taurilua_destroy(app);

    const char *inline_src =
        "local Tauri = require('tauri')\n"
        "local app = Tauri.new({})\n"
        "app.state:set('inline_worker_ran', true)\n";
    taurilua_app_t *app2 = taurilua_create_from_string(inline_src, "{}");
    assert(app2 != NULL);
    taurilua_destroy(app2);

    printf("[Smoke Test] SUCCESS: All Phase 3 embedding & concurrency checks passed!\n");
    return 0;
}
