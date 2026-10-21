#ifndef NATIVE_BRIDGE_H
#define NATIVE_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ipc_callback_fn)(const char *window_label, const char *seq, const char *req);
typedef void (*tray_callback_fn)(int id);
typedef void (*shortcut_callback_fn)(const char *shortcut);
typedef void (*deeplink_callback_fn)(const char *url);
typedef void (*menu_callback_fn)(const char *id);
typedef int (*window_close_callback_fn)(const char *label);
typedef void (*single_instance_callback_fn)(const char *payload_json);
typedef void (*theme_change_callback_fn)(const char *theme);
typedef void (*file_drop_callback_fn)(const char *label, const char *event_type, const char *payload_json);

void bridge_init(ipc_callback_fn on_ipc, tray_callback_fn on_tray, shortcut_callback_fn on_shortcut, deeplink_callback_fn on_deeplink);
void platform_menu_set_callback(menu_callback_fn on_menu);
void platform_window_set_close_callback(window_close_callback_fn on_close);
void platform_single_instance_set_callback(single_instance_callback_fn cb);
int platform_single_instance_init(const char *app_id, const char *payload_json);
void platform_single_instance_cleanup();
void platform_theme_set_callback(theme_change_callback_fn cb);
const char *platform_get_theme();
void platform_file_drop_set_callback(file_drop_callback_fn cb);
void platform_set_menu(const char *json_menu);
void platform_set_app_name(const char *name);
const char *platform_get_app_name();
void bridge_set_dev_mode(int dev);
void bridge_set_dev_url(const char *url);
void bridge_run_loop();
void bridge_terminate();

int platform_list_dir(const char *dir_path, char *out_buf, int max_len);
void platform_set_assets_path(const char *path);
int platform_is_origin_allowed(const char *url);
int platform_resolve_path(const char *in_path, char *out_path, int max_len);

int platform_window_create(const char *label, const char *title, const char *url, int width, int height, int resizable, int decorations, int transparent);
void platform_window_close(const char *label);
void platform_window_force_close(const char *label);
void platform_window_set_title(const char *label, const char *title);
void platform_window_set_size(const char *label, int width, int height);
void platform_window_minimize(const char *label);
void platform_window_maximize(const char *label);
void platform_window_unmaximize(const char *label);
void platform_window_show(const char *label);
void platform_window_hide(const char *label);
void platform_window_set_focus(const char *label);
void platform_window_start_dragging(const char *label);
void platform_set_client_script(const char *js);
void platform_window_init_script(const char *label, const char *js);
void platform_window_eval(const char *label, const char *js);
void platform_window_return_async(const char *label, const char *seq, int status, const char *result);

int platform_tray_create(const char *tooltip, const char *icon_path);
void platform_tray_set_menu(const char *json_menu);
void platform_tray_destroy();

int platform_shortcut_register(const char *shortcut);
void platform_shortcut_unregister(const char *shortcut);

int platform_deeplink_register(const char *scheme);
int platform_updater_install(const char *archive_path);

int platform_dialog_open(const char *title, const char *filter, char *out_path, int max_len);
int platform_dialog_save(const char *title, const char *filter, char *out_path, int max_len);
int platform_dialog_message(const char *title, const char *message, int is_question);
int platform_clipboard_write(const char *text);
int platform_clipboard_read(char *out_text, int max_len);
int platform_clipboard_write_html(const char *html);
int platform_clipboard_read_html(char *out_html, int max_len);
int platform_clipboard_write_image(const char *base64_png);
int platform_clipboard_read_image(char *out_base64_png, int max_len);
void platform_clipboard_clear();
void platform_notification_send(const char *title, const char *body);

void platform_state_set_string(const char *key, const char *val);
int platform_state_get_string(const char *key, char *out_val, int max_len);
void platform_state_set_number(const char *key, double val);
int platform_state_get_number(const char *key, double *out_val);
void platform_state_set_json(const char *key, const char *json_val);
int platform_state_get_json(const char *key, char *out_val, int max_len);
int platform_state_has(const char *key);
void platform_state_delete(const char *key);
void platform_state_clear();
void platform_state_lock();
void platform_state_unlock();

void platform_set_csp(const char *csp);
void platform_register_child_pid(long pid);
void platform_unregister_child_pid(long pid);
void platform_kill_child_processes();
void *platform_process_spawn(const char *program, const char **args, int arg_count);
int platform_process_read_line(void *handle, char *out_buf, int max_len);
int platform_process_wait(void *handle);
void platform_process_close(void *handle);

#ifdef __cplusplus
}
#endif

#endif