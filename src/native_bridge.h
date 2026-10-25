#pragma once
#if defined(__has_include)
  #if __has_include("taurilua.h")
    #include "taurilua.h"
  #elif __has_include("../include/taurilua.h")
    #include "../include/taurilua.h"
  #endif
#else
  #include "taurilua.h"
#endif

#ifndef TAURILUA_API
  #if defined(_WIN32)
    #define TAURILUA_API __declspec(dllexport)
  #elif defined(__GNUC__) || defined(__clang__)
    #define TAURILUA_API __attribute__((visibility("default")))
  #else
    #define TAURILUA_API
  #endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ipc_callback_fn)(const char *window_label, const char *seq, const char *req);
typedef void (*tray_callback_fn)(int id);
typedef void (*shortcut_callback_fn)(const char *shortcut);
typedef void (*deeplink_callback_fn)(const char *url);
typedef void (*menu_callback_fn)(const char *item_id);
typedef int  (*window_close_callback_fn)(const char *window_label);
typedef void (*theme_callback_fn)(const char *theme);
typedef void (*file_drop_callback_fn)(const char *window_label, const char *event_type, const char *json_payload);
typedef void (*single_instance_callback_fn)(const char *json_payload);

TAURILUA_API void bridge_init(ipc_callback_fn on_ipc, tray_callback_fn on_tray, shortcut_callback_fn on_shortcut, deeplink_callback_fn on_deeplink);
TAURILUA_API void bridge_run_loop();
TAURILUA_API void bridge_terminate();
TAURILUA_API void bridge_set_dev_mode(int dev);
TAURILUA_API void bridge_set_dev_url(const char *url);

TAURILUA_API int  platform_list_dir(const char *dir_path, char *out_buf, int max_len);
TAURILUA_API void platform_set_assets_path(const char *path);
TAURILUA_API int  platform_is_origin_allowed(const char *url);
TAURILUA_API int  platform_resolve_path(const char *in_path, char *out_path, int max_len);

TAURILUA_API int  platform_window_create(const char *label, const char *title, const char *url, int width, int height, int resizable, int decorations, int transparent);
TAURILUA_API void platform_window_close(const char *label);
TAURILUA_API void platform_window_force_close(const char *label);
TAURILUA_API void platform_window_set_title(const char *label, const char *title);
TAURILUA_API void platform_window_set_size(const char *label, int width, int height);
TAURILUA_API void platform_window_minimize(const char *label);
TAURILUA_API void platform_window_maximize(const char *label);
TAURILUA_API void platform_window_unmaximize(const char *label);
TAURILUA_API void platform_window_show(const char *label);
TAURILUA_API void platform_window_hide(const char *label);
TAURILUA_API void platform_window_set_focus(const char *label);
TAURILUA_API void platform_window_start_dragging(const char *label);
TAURILUA_API void platform_window_set_close_callback(window_close_callback_fn cb);

TAURILUA_API int  platform_single_instance_init(const char *app_id, const char *payload_json);
TAURILUA_API void platform_single_instance_cleanup();
TAURILUA_API void platform_single_instance_set_callback(single_instance_callback_fn cb);

TAURILUA_API void platform_set_client_script(const char *js);
TAURILUA_API void platform_window_init_script(const char *label, const char *js);
TAURILUA_API void platform_window_eval(const char *label, const char *js);
TAURILUA_API void platform_window_return_async(const char *label, const char *seq, int status, const char *result);

TAURILUA_API int  platform_tray_create(const char *tooltip, const char *icon_path);
TAURILUA_API void platform_tray_set_menu(const char *json_menu);
TAURILUA_API void platform_tray_destroy();

TAURILUA_API void platform_set_menu(const char *json_menu);
TAURILUA_API void platform_menu_set_callback(menu_callback_fn cb);
TAURILUA_API void platform_set_app_name(const char *name);
TAURILUA_API const char *platform_get_app_name();

TAURILUA_API int  platform_shortcut_register(const char *shortcut);
TAURILUA_API void platform_shortcut_unregister(const char *shortcut);
TAURILUA_API int  platform_deeplink_register(const char *scheme);
TAURILUA_API int  platform_updater_install(const char *archive_path);

TAURILUA_API int  platform_dialog_open(const char *title, const char *filter, char *out_path, int max_len);
TAURILUA_API int  platform_dialog_save(const char *title, const char *filter, char *out_path, int max_len);
TAURILUA_API int  platform_dialog_message(const char *title, const char *message, int is_question);

TAURILUA_API int  platform_clipboard_write(const char *text);
TAURILUA_API int  platform_clipboard_read(char *out_text, int max_len);
TAURILUA_API int  platform_clipboard_write_html(const char *html);
TAURILUA_API int  platform_clipboard_read_html(char *out_html, int max_len);
TAURILUA_API int  platform_clipboard_write_image(const char *base64_png);
TAURILUA_API int  platform_clipboard_read_image(char *out_base64_png, int max_len);
TAURILUA_API void platform_clipboard_clear();

TAURILUA_API const char *platform_get_theme();
TAURILUA_API void platform_theme_set_callback(theme_callback_fn cb);
TAURILUA_API void platform_file_drop_set_callback(file_drop_callback_fn cb);
TAURILUA_API void platform_notification_send(const char *title, const char *body);

TAURILUA_API void platform_state_set_string(const char *key, const char *val);
TAURILUA_API int  platform_state_get_string(const char *key, char *out_val, int max_len);
TAURILUA_API void platform_state_set_number(const char *key, double val);
TAURILUA_API int  platform_state_get_number(const char *key, double *out_val);
TAURILUA_API void platform_state_set_json(const char *key, const char *json_val);
TAURILUA_API int  platform_state_get_json(const char *key, char *out_val, int max_len);
TAURILUA_API int  platform_state_get_length(const char *key);
TAURILUA_API int  platform_state_has(const char *key);
TAURILUA_API void platform_state_delete(const char *key);
TAURILUA_API void platform_state_clear();
TAURILUA_API void platform_state_lock();
TAURILUA_API void platform_state_unlock();

TAURILUA_API void platform_set_csp(const char *csp);
TAURILUA_API void platform_register_child_pid(long pid);
TAURILUA_API void platform_unregister_child_pid(long pid);
TAURILUA_API void platform_kill_child_processes();

TAURILUA_API void *platform_process_spawn(const char *program, const char **args, int arg_count);
TAURILUA_API int   platform_process_read_line(void *handle, char *out_buf, int max_len);
TAURILUA_API int   platform_process_wait(void *handle);
TAURILUA_API void  platform_process_close(void *handle);

#ifdef __cplusplus
}
#endif