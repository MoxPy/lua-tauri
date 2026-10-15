#include <stdio.h>
#include <stdlib.h>
#include "taurilua.h"

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("[Host C Application] Initializing Tauri-Lua Embedded Engine...\n");

    taurilua_config_t cfg;
    taurilua_config_init(&cfg);
    cfg.title = "Host C Game Launcher";
    cfg.width = 960;
    cfg.height = 640;
    cfg.dev_mode = 0;

    // Create Tauri-Lua host instance
    taurilua_app_t *app = taurilua_create(&cfg);
    if (!app) {
        fprintf(stderr, "[Host C Error] Failed to initialize Tauri-Lua host.\n");
        return 1;
    }

    // Execute custom Lua logic from host C program
    taurilua_eval_lua(app, "print('[Host C -> Lua] Hello from Embedded C Host Engine!')");

    printf("[Host C Application] Tauri-Lua host ready. Starting window event loop...\n");
    // Run the native event loop (blocks until window is closed by user)
    int exit_code = taurilua_run(app);

    // Clean up all resources
    taurilua_destroy(app);
    printf("[Host C Application] Tauri-Lua terminated cleanly with exit code %d.\n", exit_code);
    return exit_code;
}
