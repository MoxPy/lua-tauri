#ifndef TAURILUA_H
#define TAURILUA_H

#include <stddef.h>

#define TAURILUA_VERSION_MAJOR 1
#define TAURILUA_VERSION_MINOR 0
#define TAURILUA_VERSION_PATCH 0

#if defined(TAURILUA_STATIC)
  #define TAURILUA_API
#elif defined(_WIN32) || defined(__CYGWIN__)
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

typedef struct taurilua_app taurilua_app_t;
struct lua_State;

typedef void (*taurilua_command_fn)(const char *payload_json, const char *window_label, char *out_json, size_t out_capacity, int *out_is_error, void *user_data);

typedef struct taurilua_config {
    const char *app_name;
    const char *title;
    const char *url;
    int width;
    int height;
    int resizable;
    int dev_mode;
    const char *config_json;
    const char *custom_script;
} taurilua_config_t;

TAURILUA_API void taurilua_config_init(taurilua_config_t *config);
TAURILUA_API taurilua_app_t *taurilua_create(const taurilua_config_t *config);
TAURILUA_API taurilua_app_t *taurilua_create_from_file(const char *script_path, const char *config_json);
TAURILUA_API taurilua_app_t *taurilua_create_from_string(const char *lua_source, const char *config_json);
TAURILUA_API int taurilua_run(taurilua_app_t *app);
TAURILUA_API void taurilua_terminate(taurilua_app_t *app);
TAURILUA_API void taurilua_destroy(taurilua_app_t *app);
TAURILUA_API struct lua_State *taurilua_get_lua_state(taurilua_app_t *app);
TAURILUA_API int taurilua_eval_lua(taurilua_app_t *app, const char *lua_code);
TAURILUA_API void taurilua_eval_js(taurilua_app_t *app, const char *window_label, const char *javascript);
TAURILUA_API void taurilua_emit(taurilua_app_t *app, const char *event_name, const char *payload_json);
TAURILUA_API int taurilua_register_command(taurilua_app_t *app, const char *command_name, taurilua_command_fn handler, void *user_data);
TAURILUA_API void taurilua_set_startup_payload(taurilua_app_t *app, const char *payload_json);
TAURILUA_API const char *taurilua_last_error(taurilua_app_t *app);
TAURILUA_API int taurilua_open_window(const char *title, const char *url, int width, int height);

#ifdef __cplusplus
}
#endif

#endif
