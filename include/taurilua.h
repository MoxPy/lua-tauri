#ifndef TAURILUA_H
#define TAURILUA_H

#include <stddef.h>

#if defined(_WIN32) || defined(__CYGWIN__)
  #if defined(TAURILUA_BUILD_DLL)
    #define TAURILUA_API __declspec(dllexport)
  #else
    #define TAURILUA_API __declspec(dllimport)
  #endif
#else
  #if defined(__GNUC__) && __GNUC__ >= 4
    #define TAURILUA_API __attribute__((visibility("default")))
  #else
    #define TAURILUA_API
  #endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Forward declarations */
typedef struct taurilua_app taurilua_app_t;
struct lua_State;

/**
 * Configuration for hosting and embedding Tauri-Lua
 */
typedef struct taurilua_config {
    const char *app_name;         /**< App name identifier (default: "tauri-lua-app") */
    const char *title;            /**< Window title (default: "Tauri-Lua Host") */
    const char *url;              /**< URL or frontend path (default: "" -> resolved from conf) */
    int width;                    /**< Initial window width (default: 1024) */
    int height;                   /**< Initial window height (default: 720) */
    int resizable;                /**< 1 if resizable, 0 otherwise (default: 1) */
    int dev_mode;                 /**< 1 for dev server hot reload, 0 for release (default: 0) */
    const char *config_json;      /**< Optional JSON configuration override (NULL to auto-resolve) */
    const char *custom_script;    /**< Optional path to Lua entrypoint script (NULL to auto-resolve) */
} taurilua_config_t;

/**
 * Initialize a default taurilua_config_t with recommended baseline values.
 * @param config Pointer to the struct to initialize.
 */
TAURILUA_API void taurilua_config_init(taurilua_config_t *config);

/**
 * Create a new Tauri-Lua host application instance.
 * @param config Pointer to configuration struct, or NULL for default settings.
 * @return Pointer to application instance, or NULL on failure.
 */
TAURILUA_API taurilua_app_t *taurilua_create(const taurilua_config_t *config);

/**
 * Create an instance loading a specific Lua script file.
 * @param script_path Path to the .lua script file.
 * @param config_json Optional raw JSON string for configuration (or NULL).
 * @return Pointer to application instance, or NULL on failure.
 */
TAURILUA_API taurilua_app_t *taurilua_create_from_file(const char *script_path, const char *config_json);

/**
 * Create an instance executing Lua code from a string buffer.
 * @param lua_source Lua code string to execute.
 * @param config_json Optional raw JSON string for configuration (or NULL).
 * @return Pointer to application instance, or NULL on failure.
 */
TAURILUA_API taurilua_app_t *taurilua_create_from_string(const char *lua_source, const char *config_json);

/**
 * Run the Tauri-Lua event loop.
 * This runs the native window loop (Cocoa NSApp, GTK gtk_main, or Win32 message pump)
 * until all windows are closed or taurilua_terminate is called.
 * @param app Application instance.
 * @return 0 on normal exit, non-zero on error.
 */
TAURILUA_API int taurilua_run(taurilua_app_t *app);

/**
 * Request graceful termination of the event loop.
 * @param app Application instance.
 */
TAURILUA_API void taurilua_terminate(taurilua_app_t *app);

/**
 * Destroy the application instance and free all resources (Lua state, thread pools, windows).
 * @param app Application instance.
 */
TAURILUA_API void taurilua_destroy(taurilua_app_t *app);

/**
 * Access the primary LuaJIT lua_State* for direct C-API interaction (registering C functions, pushing values).
 * @param app Application instance.
 * @return lua_State pointer, or NULL if unavailable.
 */
TAURILUA_API struct lua_State *taurilua_get_lua_state(taurilua_app_t *app);

/**
 * Execute a snippet of Lua code within the running host state.
 * @param app Application instance.
 * @param lua_code Lua code string.
 * @return 0 on success, non-zero on error.
 */
TAURILUA_API int taurilua_eval_lua(taurilua_app_t *app, const char *lua_code);

/**
 * Evaluate JavaScript in a specific webview window.
 * @param app Application instance.
 * @param window_label Target window label (e.g. "main").
 * @param javascript JavaScript code string.
 */
TAURILUA_API void taurilua_eval_js(taurilua_app_t *app, const char *window_label, const char *javascript);

/**
 * Emit a global Tauri event to all registered frontends.
 * @param app Application instance.
 * @param event_name Name of the event (e.g. "game:score").
 * @param payload_json JSON string payload.
 */
TAURILUA_API void taurilua_emit(taurilua_app_t *app, const char *event_name, const char *payload_json);

/**
 * High-level helper: Open a single Tauri-Lua webview window and run its event loop in one call.
 * Ideal for game engine launchers, settings windows, or modal dialogs.
 * @param title Window title.
 * @param url Frontend URL or file path (e.g. "dist/index.html" or "http://localhost:5173").
 * @param width Window width in pixels.
 * @param height Window height in pixels.
 * @return 0 on normal exit, non-zero on error.
 */
TAURILUA_API int taurilua_open_window(const char *title, const char *url, int width, int height);

#ifdef __cplusplus
}
#endif

#endif /* TAURILUA_H */
