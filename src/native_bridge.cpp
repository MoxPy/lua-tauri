#include "native_bridge.h"
#include "origin_check.hpp"
#include "asset_resolver.hpp"
#include "security_policy.hpp"
#include "ipc_envelope.hpp"
#include "json_parser.hpp"

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <sstream>
#include <fstream>
#include <thread>
#include <queue>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <atomic>
#include <random>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <limits>

#if defined(__APPLE__)
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#import <UserNotifications/UserNotifications.h>
#import <objc/runtime.h>
#include <sys/param.h>
#include <sys/ucred.h>
#endif

#if defined(__GNUC__) || defined(__clang__)
#define TAURILUA_INTERNAL __attribute__((visibility("hidden")))
#else
#define TAURILUA_INTERNAL
#endif

TAURILUA_INTERNAL std::atomic<bool> g_dev_mode{false};
TAURILUA_INTERNAL std::string g_dev_url = "";
TAURILUA_INTERNAL std::mutex g_config_mutex;

#define WEBVIEW_STATIC
#define WEBVIEW_IMPLEMENTATION
#include "webview.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <sddl.h>
#include <wrl.h>
#include "WebView2.h"
using namespace Microsoft::WRL;
#elif !defined(__APPLE__)
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#endif

#if !defined(_WIN32)
#include <spawn.h>
#include <signal.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/types.h>
extern char **environ;
#endif

#include <algorithm>
#include <filesystem>
#include <cctype>

#if defined(_WIN32)
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#elif !defined(__APPLE__)
#include <sys/random.h>
#endif

static std::string g_csp_policy_custom = "";

static std::string get_effective_csp() {
    std::string csp_copy;
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        csp_copy = g_csp_policy_custom;
    }
    return SecurityPolicy::build_default_csp(g_dev_mode.load(), csp_copy);
}

static std::string generate_session_token() {
    unsigned char buf[32];
    bool ok = false;
#if defined(__APPLE__)
    arc4random_buf(buf, sizeof(buf));
    ok = true;
#elif defined(_WIN32)
    NTSTATUS status = BCryptGenRandom(NULL, buf, sizeof(buf), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    ok = BCRYPT_SUCCESS(status);
#else
    ssize_t ret = getrandom(buf, sizeof(buf), 0);
    if (ret == sizeof(buf)) {
        ok = true;
    } else {
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) {
            ssize_t r = read(fd, buf, sizeof(buf));
            close(fd);
            ok = (r == sizeof(buf));
        }
    }
#endif
    if (!ok) {
        ::fprintf(stderr, "[tauri-lua] CRITICAL: System RNG failure while generating session token!\n");
        std::abort();
    }
    char hex[65];
    for (size_t i = 0; i < sizeof(buf); ++i) {
        snprintf(hex + (i * 2), 3, "%02x", buf[i]);
    }
    hex[64] = '\0';
    return std::string(hex);
}

static std::string g_session_token = generate_session_token();



static std::string get_isolation_bootstrap_script() {
    return "Object.defineProperty(window, '__TAURI_TOKEN__', {"
           "  value: '" + g_session_token + "',"
           "  writable: false,"
           "  configurable: false"
           "});";
}

enum class StateType {
    String,
    Number,
    Json,
    Bool
};

struct StateValue {
    StateType type;
    std::string str_data;
    double num_data;
    bool bool_data;
};

static std::unordered_map<std::string, StateValue> g_shared_state;
static std::unordered_map<std::string, uint64_t> g_state_versions;
static uint64_t g_state_epoch = 0;
static std::recursive_mutex g_state_mutex;

class NativeThreadPool {
public:
    NativeThreadPool(size_t threads = 4) : stop(false) {
        for (size_t i = 0; i < threads; ++i) {
            workers.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this] {
                            return this->stop || !this->tasks.empty();
                        });
                        if (this->stop && this->tasks.empty()) {
                            return;
                        }
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    task();
                }
            });
        }
    }

    void enqueue(std::function<void()> task) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            tasks.push(task);
        }
        condition.notify_one();
    }

    ~NativeThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for (std::thread &worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    std::atomic<bool> stop;
};

static NativeThreadPool g_thread_pool(std::max(2u, std::thread::hardware_concurrency()));
static ipc_callback_fn g_ipc_cb = nullptr;
static tray_callback_fn g_tray_cb = nullptr;
static shortcut_callback_fn g_shortcut_cb = nullptr;
static deeplink_callback_fn g_deeplink_cb = nullptr;
static menu_callback_fn g_menu_cb = nullptr;
static window_close_callback_fn g_window_close_cb = nullptr;

struct WindowInstance {
    std::string label;
    webview_t w;
};

static std::map<std::string, WindowInstance> g_windows;
static std::mutex g_windows_mutex;
static std::string g_assets_root = "dist";

#if defined(_WIN32)
static NOTIFYICONDATAA g_nid;
static HWND g_tray_hwnd = NULL;
static HMENU g_tray_menu = NULL;
#elif defined(__APPLE__)
static NSStatusItem *g_status_item = nil;
#endif

static inline std::string get_mime(const std::string &path) {
    return get_mime_type(path);
}

static bool read_file_bytes(const std::string &full_path, std::string &out_data) {
    static constexpr uintmax_t MAX_ASSET_BYTES = 64ULL * 1024ULL * 1024ULL;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(full_path, ec) || ec) return false;
    uintmax_t sz = std::filesystem::file_size(full_path, ec);
    if (ec || sz > MAX_ASSET_BYTES) return false;
    std::ifstream file(full_path, std::ios::binary);
    if (!file.is_open()) return false;
    out_data.resize(static_cast<size_t>(sz));
    if (sz > 0 && !file.read(&out_data[0], static_cast<std::streamsize>(sz))) {
        out_data.clear();
        return false;
    }
    return true;
}

static std::string get_assets_root_copy() {
    std::lock_guard<std::mutex> lk(g_config_mutex);
    return g_assets_root;
}

[[maybe_unused]] static std::string sanitize_app_segment(const std::string &raw) {
    std::string out;
    for (unsigned char c : raw) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.') {
            out.push_back(static_cast<char>(c));
        }
    }
    if (out.empty() || out == "." || out == ".." || out.find("..") != std::string::npos) {
        return "";
    }
    return out;
}

static file_drop_callback_fn g_file_drop_cb = nullptr;
void platform_file_drop_set_callback(file_drop_callback_fn cb) {
    g_file_drop_cb = cb;
}

static void platform_dispatch_file_drop(const char *label, const char *event_type, const std::vector<std::string> &paths, double x, double y) {
    std::string json = "{\"paths\":[";
    for (size_t i = 0; i < paths.size(); ++i) {
        if (i > 0) json += ",";
        json += json_escape_string(paths[i]);
    }
    json += "],\"position\":{\"x\":" + std::to_string((int)x) + ",\"y\":" + std::to_string((int)y) + "}}";

    if (g_file_drop_cb) {
        g_file_drop_cb(label ? label : "main", event_type ? event_type : "drop", json.c_str());
    }
}

#if defined(__APPLE__)
@interface TauriURLSchemeHandler : NSObject <WKURLSchemeHandler>
@end

@implementation TauriURLSchemeHandler
- (void)webView:(WKWebView *)webView startURLSchemeTask:(id<WKURLSchemeTask>)urlSchemeTask {
    NSURL *url = urlSchemeTask.request.URL;
    NSString *path = [url path];
    const char *utf8_path = path ? [path UTF8String] : nullptr;
    std::string req_path = utf8_path ? utf8_path : "/index.html";
    std::string assets_root = get_assets_root_copy();
    bool dev_mode = g_dev_mode.load();

    std::string full_path;
    bool found = false;
    if (dev_mode) {
        found = resolve_contained_asset(assets_root, req_path, full_path);
    }
    if (!found) {
        NSString *resPath = [[NSBundle mainBundle] resourcePath];
        const char *res_utf8 = resPath ? [resPath UTF8String] : nullptr;
        if (res_utf8) {
            std::string rel_assets = assets_root;
            while (!rel_assets.empty() && rel_assets[0] == '/') rel_assets.erase(0, 1);
            if (!rel_assets.empty() && rel_assets.find("..") == std::string::npos) {
                std::string bundle_root = std::string(res_utf8) + "/" + rel_assets;
                found = resolve_contained_asset(bundle_root, req_path, full_path);
            }
            if (!found) {
                std::string bundle_dist = std::string(res_utf8) + "/dist";
                found = resolve_contained_asset(bundle_dist, req_path, full_path);
            }
        }
    }
    if (!found && !assets_root.empty() && assets_root[0] == '/') {
        found = resolve_contained_asset(assets_root, req_path, full_path);
    }

    if (!found) {
        NSURLResponse *response = [[NSHTTPURLResponse alloc] initWithURL:url
                                                              statusCode:404
                                                             HTTPVersion:@"HTTP/1.1"
                                                            headerFields:@{
                                                                @"Content-Type": @"text/plain",
                                                                @"X-Content-Type-Options": @"nosniff"
                                                            }];
        [urlSchemeTask didReceiveResponse:response];
        NSData *err_data = [@"404 Not Found" dataUsingEncoding:NSUTF8StringEncoding];
        [urlSchemeTask didReceiveData:err_data];
        [urlSchemeTask didFinish];
        return;
    }

    std::string data;
    if (!read_file_bytes(full_path, data)) {
        NSURLResponse *response = [[NSHTTPURLResponse alloc] initWithURL:url
                                                              statusCode:404
                                                             HTTPVersion:@"HTTP/1.1"
                                                            headerFields:@{
                                                                @"Content-Type": @"text/plain",
                                                                @"X-Content-Type-Options": @"nosniff"
                                                            }];
        [urlSchemeTask didReceiveResponse:response];
        [urlSchemeTask didFinish];
        return;
    }

    std::string mime = get_mime(full_path);
    std::string csp = get_effective_csp();

    if (mime == "text/html") {
        inject_csp_meta(data, csp);
    }

    NSURLResponse *response = [[NSHTTPURLResponse alloc] initWithURL:url
                                                          statusCode:200
                                                         HTTPVersion:@"HTTP/1.1"
                                                        headerFields:@{
                                                            @"Content-Type": webview_nsstring_utf8(mime),
                                                            @"X-Content-Type-Options": @"nosniff",
                                                            @"Content-Security-Policy": webview_nsstring_utf8(csp)
                                                        }];

    [urlSchemeTask didReceiveResponse:response];
    NSData *nsdata = [NSData dataWithBytes:data.data() length:data.size()];
    [urlSchemeTask didReceiveData:nsdata];
    [urlSchemeTask didFinish];
}

- (void)webView:(WKWebView *)webView stopURLSchemeTask:(id<WKURLSchemeTask>)urlSchemeTask {
}
@end



@interface TauriWKWebView : WKWebView
@end

@implementation TauriWKWebView
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    NSPasteboard *pboard = [sender draggingPasteboard];
    NSArray *urls = [pboard readObjectsForClasses:@[[NSURL class]] options:nil];
    if (urls && [urls count] > 0) {
        std::vector<std::string> paths;
        for (NSURL *url in urls) {
            if ([url isFileURL]) {
                NSString *up = [url path];
                const char *utf8_p = up ? [up UTF8String] : nullptr;
                if (utf8_p && utf8_p[0] != '\0') {
                    paths.push_back(utf8_p);
                }
            }
        }
        if (!paths.empty()) {
            NSPoint pt = [sender draggingLocation];
            NSWindow *win = [self window];
            double y = win ? (win.frame.size.height - pt.y) : pt.y;
            platform_dispatch_file_drop("main", "drop", paths, pt.x, y);
            return YES;
        }
    }
    return [super performDragOperation:sender];
}
@end


#endif

#if !defined(_WIN32) && !defined(__APPLE__)
static void gtk_uri_scheme_request_cb(WebKitURISchemeRequest *request, gpointer user_data) {
    (void)user_data;
    const gchar *path_str = webkit_uri_scheme_request_get_path(request);
    std::string req_path = (path_str && strlen(path_str) > 0) ? path_str : "/index.html";
    std::string assets_root = get_assets_root_copy();
    bool dev_mode = g_dev_mode.load();

    std::string full_path;
    bool found = false;
    if (dev_mode) {
        found = resolve_contained_asset(assets_root, req_path, full_path);
    }
    if (!found) {
        std::vector<std::string> search_roots;
        std::string rel_assets = assets_root;
        while (!rel_assets.empty() && rel_assets[0] == '/') rel_assets.erase(0, 1);
        if (rel_assets.empty() || rel_assets.find("..") != std::string::npos) {
            rel_assets = "dist";
        }
        if (!assets_root.empty() && assets_root[0] == '/') {
            search_roots.push_back(assets_root);
        }
        std::vector<char> exe_buf(4096, '\0');
        ssize_t elen = readlink("/proc/self/exe", exe_buf.data(), exe_buf.size() - 1);
        if (elen > 0 && static_cast<size_t>(elen) < exe_buf.size() - 1) {
            exe_buf[static_cast<size_t>(elen)] = '\0';
            std::string ep(exe_buf.data());
            size_t slash = ep.rfind('/');
            if (slash != std::string::npos) {
                std::string exe_name = sanitize_app_segment(ep.substr(slash + 1));
                std::string exe_dir = ep.substr(0, slash);
                if (!exe_name.empty()) {
                    search_roots.push_back("/usr/share/" + exe_name + "/" + rel_assets);
                    search_roots.push_back("/usr/local/share/" + exe_name + "/" + rel_assets);
                    search_roots.push_back(exe_dir + "/../share/" + exe_name + "/" + rel_assets);
                }
                search_roots.push_back(exe_dir + "/" + rel_assets);
            }
        }
        std::string app_copy;
        {
            std::lock_guard<std::mutex> lk(g_config_mutex);
            app_copy = g_app_name;
        }
        if (!app_copy.empty()) {
            std::string slug = app_copy;
            for (char &c : slug) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (char &c : slug) if (c == ' ' || c == '_') c = '-';
            slug = sanitize_app_segment(slug);
            std::string safe_app = sanitize_app_segment(app_copy);
            if (!slug.empty()) {
                search_roots.push_back("/usr/share/" + slug + "/" + rel_assets);
                search_roots.push_back("/usr/local/share/" + slug + "/" + rel_assets);
            }
            if (!safe_app.empty() && safe_app != slug) {
                search_roots.push_back("/usr/share/" + safe_app + "/" + rel_assets);
            }
        }

        for (const auto &root : search_roots) {
            if (resolve_contained_asset(root, req_path, full_path)) {
                found = true;
                break;
            }
        }
    }

    if (!found) {
        GError *error = g_error_new_literal(G_FILE_ERROR, G_FILE_ERROR_NOENT, "404 Not Found");
        webkit_uri_scheme_request_finish_error(request, error);
        g_error_free(error);
        return;
    }

    std::string data;
    if (!read_file_bytes(full_path, data)) {
        GError *error = g_error_new_literal(G_FILE_ERROR, G_FILE_ERROR_NOENT, "404 Not Found");
        webkit_uri_scheme_request_finish_error(request, error);
        g_error_free(error);
        return;
    }

    std::string mime = get_mime(full_path);
    std::string csp = get_effective_csp();

    if (mime == "text/html") {
        inject_csp_meta(data, csp);
    }

    gpointer owned_buf = g_malloc(data.empty() ? 1 : data.size());
    if (!data.empty()) {
        std::memcpy(owned_buf, data.data(), data.size());
    }
    GInputStream *stream = g_memory_input_stream_new_from_data(owned_buf, static_cast<gssize>(data.size()), g_free);
#if defined(WEBKIT_CHECK_VERSION) && WEBKIT_CHECK_VERSION(2, 36, 0)
    WebKitURISchemeResponse *resp = webkit_uri_scheme_response_new(stream, static_cast<gint64>(data.size()));
    webkit_uri_scheme_response_set_status(resp, 200, "OK");
    webkit_uri_scheme_response_set_content_type(resp, mime.c_str());
    SoupMessageHeaders *headers = soup_message_headers_new(SOUP_MESSAGE_HEADERS_RESPONSE);
    soup_message_headers_append(headers, "X-Content-Type-Options", "nosniff");
    if (!csp.empty()) {
        soup_message_headers_append(headers, "Content-Security-Policy", csp.c_str());
    }
    webkit_uri_scheme_response_set_http_headers(resp, headers);
    webkit_uri_scheme_request_finish_with_response(request, resp);
    g_object_unref(resp);
#else
    webkit_uri_scheme_request_finish(request, stream, static_cast<gint64>(data.size()), mime.c_str());
#endif
    g_object_unref(stream);
}
#endif

static bool get_native_window_origin(webview_t w, std::string &out_origin) {
    if (!w) return false;
    void *h = webview_get_window(w);
    if (!h) return false;

#if defined(_WIN32)
    ICoreWebView2Controller *controller = static_cast<ICoreWebView2Controller *>(
        webview_get_native_handle(w, WEBVIEW_NATIVE_HANDLE_KIND_BROWSER_CONTROLLER));
    if (!controller) return false;

    ICoreWebView2 *core = nullptr;
    if (SUCCEEDED(controller->get_CoreWebView2(&core)) && core) {
        LPWSTR uri_w = nullptr;
        if (SUCCEEDED(core->get_Source(&uri_w)) && uri_w) {
            int len = WideCharToMultiByte(CP_UTF8, 0, uri_w, -1, NULL, 0, NULL, NULL);
            if (len > 0) {
                std::string u8(len, '\0');
                int written = WideCharToMultiByte(CP_UTF8, 0, uri_w, -1, &u8[0], len, NULL, NULL);
                if (written > 0) {
                    u8.resize(static_cast<size_t>(written - 1));
                    out_origin = std::move(u8);
                } else {
                    out_origin.clear();
                }
            } else {
                out_origin.clear();
            }
            CoTaskMemFree(uri_w);
            core->Release();
            return !out_origin.empty();
        }
        core->Release();
    }
    return false;

#elif defined(__APPLE__)
    __block bool ok = false;
    __block std::string found_url;
    run_on_main_sync(^{
        NSWindow *nswin = (__bridge NSWindow *)h;
        NSView *content = [nswin contentView];
        if ([content isKindOfClass:[WKWebView class]]) {
            WKWebView *wk = (WKWebView *)content;
            NSURL *url = wk.URL;
            if (url && url.absoluteString) {
                found_url = [[url absoluteString] UTF8String];
                ok = true;
            }
        }
    });
    if (ok) {
        out_origin = found_url;
    }
    return ok;

#else
    void *wk_ptr = webview_get_native_handle(w, WEBVIEW_NATIVE_HANDLE_KIND_UI_WIDGET);
    if (wk_ptr && WEBKIT_IS_WEB_VIEW(GTK_WIDGET(wk_ptr))) {
        const gchar *uri = webkit_web_view_get_uri(WEBKIT_WEB_VIEW(wk_ptr));
        if (uri) {
            out_origin = std::string(uri);
            return true;
        }
    }
    return false;
#endif
}

extern "C" {

void platform_set_assets_path(const char *path) {
    if (path && strlen(path) > 0) {
        std::lock_guard<std::mutex> lk(g_config_mutex);
        g_assets_root = path;
    }
}

void bridge_set_dev_mode(int dev) {
    g_dev_mode.store(dev != 0);
}

void bridge_set_dev_url(const char *url) {
    std::lock_guard<std::mutex> lock(g_config_mutex);
    g_dev_url = url ? url : "";
}

int platform_list_dir(const char *dir_path, char *out_buf, int max_len) {
    if (!dir_path || !out_buf || max_len <= 0) return 0;
    out_buf[0] = '\0';
    try {
        std::filesystem::path p(dir_path);
        std::error_code ec;
        if (!std::filesystem::exists(p, ec) || !std::filesystem::is_directory(p, ec)) {
            return 0;
        }
        std::vector<std::string> entries;
        for (const auto &entry : std::filesystem::directory_iterator(p, ec)) {
            if (entry.is_regular_file(ec)) {
                entries.push_back(entry.path().filename().string());
            }
        }
        std::sort(entries.begin(), entries.end());
        std::string joined;
        for (const auto &name : entries) {
            joined += name + "\n";
        }
        int copy_len = std::min(max_len - 1, static_cast<int>(joined.size()));
        memcpy(out_buf, joined.data(), copy_len);
        out_buf[copy_len] = '\0';
        return copy_len;
    } catch (...) {
        return 0;
    }
}

int platform_read_dir(const char *dir_path, char *out_buf, int max_len) {
    if (!dir_path || !out_buf || max_len <= 0) return -1;
    out_buf[0] = '\0';
    try {
        std::filesystem::path p(dir_path);
        std::error_code ec;
        if (!std::filesystem::exists(p, ec) || !std::filesystem::is_directory(p, ec)) {
            return -1;
        }
        std::string json = "[";
        size_t count = 0;
        static constexpr size_t MAX_DIR_ENTRIES = 100000;
        for (const auto &entry : std::filesystem::directory_iterator(p, ec)) {
            if (ec) break;
            if (count >= MAX_DIR_ENTRIES) break;
            std::string name = entry.path().filename().string();
            if (name == "." || name == "..") continue;
            std::error_code dec;
            bool is_dir = entry.is_directory(dec) && !dec;
            if (count > 0) json += ",";
            json += "{\"name\":" + json_escape_string(name) + ",\"isDirectory\":" + (is_dir ? "true" : "false") + "}";
            count++;
        }
        json += "]";
        if (json.size() >= static_cast<size_t>(max_len)) {
            return static_cast<int>(json.size() + 1);
        }
        memcpy(out_buf, json.data(), json.size());
        out_buf[json.size()] = '\0';
        return 1;
    } catch (...) {
        return -1;
    }
}

int platform_fs_write_file(const char *path, const char *data, int data_len) {
    if (!path || path[0] == '\0' || data_len < 0) return 0;
    try {
        std::error_code ec;
        std::filesystem::path p(path);
        std::filesystem::file_status st = std::filesystem::symlink_status(p, ec);
        if (!ec && std::filesystem::is_symlink(st)) {
            return 0;
        }
#if defined(_WIN32)
        int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
        if (wlen <= 0) return 0;
        std::wstring wpath(static_cast<size_t>(wlen), 0);
        MultiByteToWideChar(CP_UTF8, 0, path, -1, &wpath[0], wlen);
        DWORD attrs = GetFileAttributesW(wpath.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
            return 0;
        }
        HANDLE h = CreateFileW(wpath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) return 0;
        DWORD total_written = 0;
        while (total_written < static_cast<DWORD>(data_len)) {
            DWORD chunk = 0;
            if (!WriteFile(h, data + total_written, static_cast<DWORD>(data_len) - total_written, &chunk, NULL)) {
                CloseHandle(h);
                return 0;
            }
            total_written += chunk;
        }
        CloseHandle(h);
        return 1;
#else
        int flags = O_WRONLY | O_CREAT | O_TRUNC;
#if defined(O_NOFOLLOW)
        flags |= O_NOFOLLOW;
#endif
        int fd = ::open(path, flags, 0600);
        if (fd < 0) return 0;
        size_t total = 0;
        size_t target = static_cast<size_t>(data_len);
        while (total < target) {
            ssize_t w = ::write(fd, data + total, target - total);
            if (w < 0) {
                if (errno == EINTR) continue;
                ::close(fd);
                return 0;
            }
            total += static_cast<size_t>(w);
        }
        ::close(fd);
        return 1;
#endif
    } catch (...) {
        return 0;
    }
}

static constexpr size_t MAX_TRACKED_PIDS = 64;
static std::atomic<long> g_tracked_pids[MAX_TRACKED_PIDS] = {};

extern "C" void platform_register_child_pid(long pid) {
    if (pid <= 0 || pid > static_cast<long>(std::numeric_limits<int32_t>::max())) return;
    for (size_t i = 0; i < MAX_TRACKED_PIDS; ++i) {
        if (g_tracked_pids[i].load(std::memory_order_relaxed) == pid) return;
    }
    for (size_t i = 0; i < MAX_TRACKED_PIDS; ++i) {
        long expected = 0;
        if (g_tracked_pids[i].compare_exchange_strong(expected, pid, std::memory_order_relaxed)) {
            break;
        }
    }
}

extern "C" void platform_unregister_child_pid(long pid) {
    if (pid <= 0 || pid > static_cast<long>(std::numeric_limits<int32_t>::max())) return;
    for (size_t i = 0; i < MAX_TRACKED_PIDS; ++i) {
        long expected = pid;
        g_tracked_pids[i].compare_exchange_strong(expected, 0, std::memory_order_relaxed);
    }
}

extern "C" void platform_kill_child_processes() {
    for (size_t i = 0; i < MAX_TRACKED_PIDS; ++i) {
        long pid = g_tracked_pids[i].exchange(0, std::memory_order_relaxed);
        if (pid > 0 && pid <= static_cast<long>(std::numeric_limits<int32_t>::max())) {
#if defined(_WIN32)
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
            if (h) {
                TerminateProcess(h, 1);
                CloseHandle(h);
            }
#else
            ::kill(static_cast<pid_t>(pid), SIGTERM);
            ::kill(static_cast<pid_t>(pid), SIGKILL);
#endif
        }
    }
}

void bridge_init(ipc_callback_fn on_ipc, tray_callback_fn on_tray, shortcut_callback_fn on_shortcut, deeplink_callback_fn on_deeplink) {
    atexit(platform_kill_child_processes);
    g_ipc_cb = on_ipc;
    g_tray_cb = on_tray;
    g_shortcut_cb = on_shortcut;
    g_deeplink_cb = on_deeplink;

#if !defined(_WIN32) && !defined(__APPLE__)
    WebKitWebContext *context = webkit_web_context_get_default();
    webkit_web_context_register_uri_scheme(context, "tauri", gtk_uri_scheme_request_cb, NULL, NULL);
    webkit_web_context_register_uri_scheme(context, "asset", gtk_uri_scheme_request_cb, NULL, NULL);
#endif
}

struct ReturnPayload {
    webview_t w;
    std::string seq;
    int status;
    std::string result;
};

static void dispatch_return_cb(webview_t w, void *arg) {
    ReturnPayload *p = static_cast<ReturnPayload *>(arg);
    webview_return(p->w, p->seq.c_str(), p->status, p->result.c_str());
    delete p;
}

void platform_window_return_async(const char *label, const char *seq, int status, const char *result) {
    if (!seq || !is_valid_seq_id(seq)) return;
    std::lock_guard<std::mutex> lock(g_windows_mutex);
    auto it = g_windows.find(label);
    if (it != g_windows.end()) {
        ReturnPayload *p = new ReturnPayload();
        p->w = it->second.w;
        p->seq = seq;
        p->status = status;
        p->result = result ? result : "null";
        webview_dispatch(it->second.w, dispatch_return_cb, p);
    }
}

static void common_bind_handler(const char *seq, const char *req, void *arg) {
    const char *label_ptr = static_cast<const char *>(arg);
    std::string label = label_ptr ? label_ptr : "main";
    std::string s_seq = seq ? seq : "";
    if (!is_valid_seq_id(s_seq)) {
        return;
    }
    std::string s_req = req ? req : "";

    webview_t target_w = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_windows_mutex);
        auto it = g_windows.find(label);
        if (it != g_windows.end()) {
            target_w = it->second.w;
        }
    }

    if (!target_w) {
        return;
    }

    std::string current_origin;
    std::string dev_url_copy;
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        dev_url_copy = g_dev_url;
    }
    if (!get_native_window_origin(target_w, current_origin) || !OriginValidator::is_allowed(current_origin, g_dev_mode.load(), dev_url_copy)) {
        std::string err_payload = "{\"error\":\"ERR_ORIGIN_DENIED: Unauthorized origin (" + current_origin + ")\"}";
        platform_window_return_async(label.c_str(), s_seq.c_str(), 1, err_payload.c_str());
        return;
    }

    if (!verify_envelope_token(s_req, g_session_token)) {
        std::string err_payload = "{\"error\":\"ERR_TOKEN_INVALID: Missing or invalid isolation token\"}";
        platform_window_return_async(label.c_str(), s_seq.c_str(), 1, err_payload.c_str());
        return;
    }

    g_thread_pool.enqueue([label, s_seq, s_req]() {
        if (g_ipc_cb) {
            g_ipc_cb(label.c_str(), s_seq.c_str(), s_req.c_str());
        }
    });
}

#if defined(_WIN32)
void win32_attach_current_menu(HWND hwnd);
#elif !defined(__APPLE__)
void gtk_attach_current_menu(GtkWindow *gtkwin);
#endif

static int on_internal_webview_close(webview_t w, void *arg) {
    const char *label = static_cast<const char *>(arg);
    if (g_window_close_cb && label) {
        return g_window_close_cb(label);
    }
    return 0; // Allow close
}

static std::string g_default_client_script = "";

void platform_set_client_script(const char *js) {
    std::lock_guard<std::mutex> lock(g_windows_mutex);
    if (js) {
        g_default_client_script = js;
    }
}

#if !defined(_WIN32) && !defined(__APPLE__)
static void run_on_gtk_main(std::function<void()> fn) {
    if (g_main_context_is_owner(g_main_context_default())) {
        fn();
        return;
    }
    auto *heap_fn = new std::function<void()>(std::move(fn));
    g_main_context_invoke(NULL, +[](gpointer data) -> gboolean {
        auto *f = static_cast<std::function<void()> *>(data);
        if (f) {
            (*f)();
            delete f;
        }
        return G_SOURCE_REMOVE;
    }, heap_fn);
}
#endif

static webview_t lookup_window_webview(const char *label, bool fallback_first = false) {
    std::lock_guard<std::mutex> lock(g_windows_mutex);
    auto it = g_windows.find(label ? label : "main");
    if (it == g_windows.end() && fallback_first && !g_windows.empty()) {
        it = g_windows.begin();
    }
    return (it != g_windows.end()) ? it->second.w : nullptr;
}

int platform_window_create(const char *label, const char *title, const char *url, int width, int height, int resizable, int decorations, int transparent) {
    std::string s_label = label ? label : "main";
    std::string default_client_script_copy;
    {
        std::lock_guard<std::mutex> lock(g_windows_mutex);
        if (g_windows.find(s_label) != g_windows.end()) {
            return -1;
        }
        default_client_script_copy = g_default_client_script;
    }

    webview_t w = webview_create(g_dev_mode.load() ? 1 : 0, nullptr);
    if (!w) return -1;

    webview_set_close_handler(w, on_internal_webview_close, strdup(s_label.c_str()));

    webview_set_title(w, title);
    webview_set_size(w, width, height, resizable ? 0 : 3);

    std::string target_url = url ? url : "";
    std::string d_url;
    {
        std::lock_guard<std::mutex> lk(g_config_mutex);
        d_url = g_dev_url;
    }
    bool is_trusted_window = target_url.empty() || OriginValidator::is_allowed(target_url, g_dev_mode.load(), d_url);
    if (is_trusted_window) {
        webview_init(w, get_isolation_bootstrap_script().c_str());
        if (!default_client_script_copy.empty()) {
            webview_init(w, default_client_script_copy.c_str());
        }
        char *label_ctx = strdup(s_label.c_str());
        webview_bind(w, "__TAURI_RAW_INVOKE__", common_bind_handler, label_ctx);
    }

    void *native_handle = webview_get_window(w);

#if defined(_WIN32)
    HWND hwnd = static_cast<HWND>(native_handle);
    if (!decorations) {
        LONG style = GetWindowLong(hwnd, GWL_STYLE);
        style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
        SetWindowLong(hwnd, GWL_STYLE, style);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    if (transparent) {
        LONG ex_style = GetWindowLong(hwnd, GWL_EXSTYLE);
        SetWindowLong(hwnd, GWL_EXSTYLE, ex_style | WS_EX_LAYERED);
    }

    ICoreWebView2Controller *controller = static_cast<ICoreWebView2Controller *>(webview_get_native_handle(w, WEBVIEW_NATIVE_HANDLE_KIND_BROWSER_CONTROLLER));
    if (controller) {
        ICoreWebView2 *core = nullptr;
        controller->get_CoreWebView2(&core);
        if (core) {
            ICoreWebView2_3 *core3 = nullptr;
            core->QueryInterface(IID_PPV_ARGS(&core3));
            if (core3) {
                std::string root_copy = get_assets_root_copy();
                int wlen = MultiByteToWideChar(CP_UTF8, 0, root_copy.c_str(), -1, NULL, 0);
                if (wlen > 0) {
                    std::wstring wroot(static_cast<size_t>(wlen), 0);
                    MultiByteToWideChar(CP_UTF8, 0, root_copy.c_str(), -1, &wroot[0], wlen);
                    core3->SetVirtualHostNameToFolderMapping(L"tauri.localhost", wroot.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
                }
                core3->Release();
            }
            core->Release();
        }
    }
#elif defined(__APPLE__)
    run_on_main_sync(^{
        NSWindow *nswin = (__bridge NSWindow *)native_handle;
        if (!decorations) {
            [nswin setStyleMask:NSWindowStyleMaskBorderless];
        }
        if (transparent) {
            [nswin setOpaque:NO];
            [nswin setBackgroundColor:[NSColor clearColor]];
        }
    });
#else
    GtkWindow *gtkwin = GTK_WINDOW(native_handle);
    if (!decorations) {
        gtk_window_set_decorated(gtkwin, FALSE);
    }
    if (transparent) {
        GdkScreen *screen = gtk_widget_get_screen(GTK_WIDGET(gtkwin));
        GdkVisual *visual = gdk_screen_get_rgba_visual(screen);
        if (visual) {
            gtk_widget_set_visual(GTK_WIDGET(gtkwin), visual);
            gtk_widget_set_app_paintable(GTK_WIDGET(gtkwin), TRUE);
        }
    }
#endif

    {
        std::lock_guard<std::mutex> lock(g_windows_mutex);
        g_windows[s_label] = WindowInstance{s_label, w};
    }

    webview_navigate(w, url);

#if defined(_WIN32)
    win32_attach_current_menu(static_cast<HWND>(native_handle));
    ShowWindow(static_cast<HWND>(native_handle), SW_SHOW);
    UpdateWindow(static_cast<HWND>(native_handle));
#elif defined(__APPLE__)
    run_on_main_sync(^{
        NSWindow *nswin = (__bridge NSWindow *)native_handle;
        [nswin center];
        [nswin makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    });
#else
    gtk_attach_current_menu(GTK_WINDOW(native_handle));
    gtk_widget_show_all(GTK_WIDGET(native_handle));
#endif

    return 0;
}

void platform_window_close(const char *label) {
    webview_t w = lookup_window_webview(label);
    if (!w) return;
    void *h = webview_get_window(w);
#if defined(__APPLE__)
    run_on_main_async(^{
        [(__bridge NSWindow *)h performClose:nil];
    });
#elif defined(_WIN32)
    PostMessage(static_cast<HWND>(h), WM_CLOSE, 0, 0);
#else
    run_on_gtk_main([h]() {
        gtk_window_close(GTK_WINDOW(h));
    });
#endif
}

void platform_window_force_close(const char *label) {
    webview_t w = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_windows_mutex);
        auto it = g_windows.find(label ? label : "main");
        if (it != g_windows.end()) {
            w = it->second.w;
            g_windows.erase(it);
        }
    }
    if (w) {
#if !defined(_WIN32) && !defined(__APPLE__)
        run_on_gtk_main([w]() { webview_force_close(w); });
#else
        webview_force_close(w);
#endif
    }
}

void platform_window_set_title(const char *label, const char *title) {
    webview_t w = lookup_window_webview(label);
    if (!w || !title) return;
    std::string s_title(title);
#if !defined(_WIN32) && !defined(__APPLE__)
    run_on_gtk_main([w, s_title]() { webview_set_title(w, s_title.c_str()); });
#else
    webview_set_title(w, s_title.c_str());
#endif
}

void platform_window_set_size(const char *label, int width, int height) {
    webview_t w = lookup_window_webview(label);
    if (!w) return;
#if !defined(_WIN32) && !defined(__APPLE__)
    run_on_gtk_main([w, width, height]() { webview_set_size(w, width, height, 0); });
#else
    webview_set_size(w, width, height, 0);
#endif
}

void platform_window_minimize(const char *label) {
    webview_t w = lookup_window_webview(label);
    if (!w) return;
    void *h = webview_get_window(w);
#if defined(_WIN32)
    ShowWindowAsync(static_cast<HWND>(h), SW_MINIMIZE);
#elif defined(__APPLE__)
    run_on_main_async(^{
        [(__bridge NSWindow *)h miniaturize:nil];
    });
#else
    run_on_gtk_main([h]() { gtk_window_iconify(GTK_WINDOW(h)); });
#endif
}

void platform_window_maximize(const char *label) {
    webview_t w = lookup_window_webview(label);
    if (!w) return;
    void *h = webview_get_window(w);
#if defined(_WIN32)
    ShowWindowAsync(static_cast<HWND>(h), SW_MAXIMIZE);
#elif defined(__APPLE__)
    run_on_main_async(^{
        [(__bridge NSWindow *)h zoom:nil];
    });
#else
    run_on_gtk_main([h]() { gtk_window_maximize(GTK_WINDOW(h)); });
#endif
}

void platform_window_unmaximize(const char *label) {
    webview_t w = lookup_window_webview(label);
    if (!w) return;
    void *h = webview_get_window(w);
#if defined(_WIN32)
    ShowWindowAsync(static_cast<HWND>(h), SW_RESTORE);
#elif defined(__APPLE__)
    run_on_main_async(^{
        [(__bridge NSWindow *)h zoom:nil];
    });
#else
    run_on_gtk_main([h]() { gtk_window_unmaximize(GTK_WINDOW(h)); });
#endif
}

void platform_window_show(const char *label) {
    webview_t w = lookup_window_webview(label);
    if (!w) return;
    void *h = webview_get_window(w);
#if defined(_WIN32)
    ShowWindowAsync(static_cast<HWND>(h), SW_SHOW);
#elif defined(__APPLE__)
    run_on_main_async(^{
        [(__bridge NSWindow *)h makeKeyAndOrderFront:nil];
    });
#else
    run_on_gtk_main([h]() { gtk_widget_show_all(GTK_WIDGET(h)); });
#endif
}

void platform_window_hide(const char *label) {
    webview_t w = lookup_window_webview(label);
    if (!w) return;
    void *h = webview_get_window(w);
#if defined(_WIN32)
    ShowWindowAsync(static_cast<HWND>(h), SW_HIDE);
#elif defined(__APPLE__)
    run_on_main_async(^{
        [(__bridge NSWindow *)h orderOut:nil];
    });
#else
    run_on_gtk_main([h]() { gtk_widget_hide(GTK_WIDGET(h)); });
#endif
}

void platform_window_set_focus(const char *label) {
    webview_t w = lookup_window_webview(label, true);
    if (!w) return;
    void *h = webview_get_window(w);
#if defined(_WIN32)
    HWND hwnd = static_cast<HWND>(h);
    if (IsIconic(hwnd)) ShowWindowAsync(hwnd, SW_RESTORE);
    ShowWindowAsync(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
#elif defined(__APPLE__)
    run_on_main_async(^{
        NSWindow *win = (__bridge NSWindow *)h;
        [NSApp activateIgnoringOtherApps:YES];
        if ([win isMiniaturized]) {
            [win deminiaturize:nil];
        }
        [win makeKeyAndOrderFront:nil];
    });
#else
    run_on_gtk_main([h]() {
        gtk_window_deiconify(GTK_WINDOW(h));
        gtk_window_present(GTK_WINDOW(h));
    });
#endif
}

void platform_window_start_dragging(const char *label) {
    webview_t w = lookup_window_webview(label);
    if (!w) return;
    void *h = webview_get_window(w);
#if defined(_WIN32)
    HWND hwnd = static_cast<HWND>(h);
    PostMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
#elif defined(__APPLE__)
    run_on_main_async(^{
        NSWindow *nswin = (__bridge NSWindow *)h;
        NSEvent *event = [NSApp currentEvent];
        if (event) {
            [nswin performWindowDragWithEvent:event];
        }
    });
#else
    run_on_gtk_main([h]() {
        GtkWindow *gtkwin = GTK_WINDOW(h);
        gtk_window_begin_move_drag(gtkwin, 1, 0, 0, GDK_CURRENT_TIME);
    });
#endif
}

struct EvalPayload {
    webview_t w;
    std::string js;
    bool check_origin;
};

static void dispatch_eval_cb(webview_t w, void *arg) {
    (void)w;
    EvalPayload *p = static_cast<EvalPayload *>(arg);
    if (!p) return;
    if (p->check_origin) {
        std::string current_origin;
        std::string dev_url_copy;
        {
            std::lock_guard<std::mutex> lock(g_config_mutex);
            dev_url_copy = g_dev_url;
        }
        if (get_native_window_origin(p->w, current_origin) && OriginValidator::is_allowed(current_origin, g_dev_mode.load(), dev_url_copy)) {
            webview_eval(p->w, p->js.c_str());
        }
    } else {
        webview_eval(p->w, p->js.c_str());
    }
    delete p;
}

void platform_window_init_script(const char *label, const char *js) {
    if (!js) return;
    std::vector<webview_t> targets;
    {
        std::lock_guard<std::mutex> lock(g_windows_mutex);
        if (label && strcmp(label, "*") == 0) {
            for (auto &pair : g_windows) {
                targets.push_back(pair.second.w);
            }
        } else {
            std::string s_label = label ? label : "main";
            auto it = g_windows.find(s_label);
            if (it != g_windows.end()) {
                targets.push_back(it->second.w);
            }
        }
    }
    std::string s_js(js);
    for (webview_t w : targets) {
#if !defined(_WIN32) && !defined(__APPLE__)
        run_on_gtk_main([w, s_js]() { webview_init(w, s_js.c_str()); });
#else
        webview_init(w, s_js.c_str());
#endif
    }
}

void platform_window_eval(const char *label, const char *js) {
    if (!label || !js) return;
    std::vector<std::pair<webview_t, bool>> targets;
    {
        std::lock_guard<std::mutex> lock(g_windows_mutex);
        if (strcmp(label, "*") == 0) {
            for (auto &pair : g_windows) {
                targets.push_back({pair.second.w, true});
            }
        } else {
            auto it = g_windows.find(label);
            if (it != g_windows.end()) {
                targets.push_back({it->second.w, false});
            }
        }
    }
    for (auto &tgt : targets) {
        EvalPayload *p = new EvalPayload();
        p->w = tgt.first;
        p->js = js;
        p->check_origin = tgt.second;
        webview_dispatch(tgt.first, dispatch_eval_cb, p);
    }
}

void bridge_run_loop() {
    webview_t first_w = lookup_window_webview("main", true);
    if (first_w) {
        webview_run(first_w);
    }
}

void bridge_terminate() {
    platform_single_instance_cleanup();
    std::vector<webview_t> targets;
    {
        std::lock_guard<std::mutex> lock(g_windows_mutex);
        for (auto &pair : g_windows) {
            targets.push_back(pair.second.w);
        }
    }
    for (webview_t w : targets) {
        webview_terminate(w);
    }
}

#if defined(_WIN32)
static LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_USER + 1) {
        if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd);
            int cmd = TrackPopupMenu(g_tray_menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
            if (cmd > 0 && g_tray_cb) {
                g_tray_cb(cmd);
            }
        }
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}
#endif

int platform_tray_create(const char *tooltip, const char *icon_path) {
#if defined(_WIN32)
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "TauriLuaTrayClass";
    RegisterClassExA(&wc);

    g_tray_hwnd = CreateWindowExA(0, "TauriLuaTrayClass", "", 0, 0, 0, 0, 0, NULL, NULL, GetModuleHandle(NULL), NULL);

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_tray_hwnd;
    g_nid.uID = 1001;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_USER + 1;
    g_nid.hIcon = (HICON)LoadImageA(NULL, icon_path ? icon_path : "", IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
    if (!g_nid.hIcon) {
        g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    }
    strncpy(g_nid.szTip, tooltip ? tooltip : "Tauri Tray", sizeof(g_nid.szTip) - 1);
    Shell_NotifyIconA(NIM_ADD, &g_nid);
    g_tray_menu = CreatePopupMenu();
    return 1;
#elif defined(__APPLE__)
    std::string s_tip = tooltip ? tooltip : "T";
    run_on_main_sync(^{
        g_status_item = [[NSStatusBar systemStatusBar] statusItemWithLength:NSSquareStatusItemLength];
        g_status_item.button.title = webview_nsstring_utf8(s_tip);
    });
    return 1;
#else
    return 1;
#endif
}

void platform_tray_set_menu(const char *json_menu) {
#if defined(_WIN32)
    if (!g_tray_menu) return;
    DestroyMenu(g_tray_menu);
    g_tray_menu = CreatePopupMenu();
    AppendMenuA(g_tray_menu, MF_STRING, 1, "Open Window");
    AppendMenuA(g_tray_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(g_tray_menu, MF_STRING, 2, "Quit");
#endif
}

void platform_tray_destroy() {
#if defined(_WIN32)
    if (g_tray_hwnd) {
        Shell_NotifyIconA(NIM_DELETE, &g_nid);
        DestroyWindow(g_tray_hwnd);
        g_tray_hwnd = NULL;
    }
#elif defined(__APPLE__)
    run_on_main_sync(^{
        if (g_status_item) {
            [[NSStatusBar systemStatusBar] removeStatusItem:g_status_item];
            g_status_item = nil;
        }
    });
#endif
}

int platform_shortcut_register(const char *shortcut) {
#if defined(_WIN32)
    UINT mod = MOD_CONTROL | MOD_ALT;
    UINT vk = 'T';
    return RegisterHotKey(NULL, 101, mod, vk) ? 1 : 0;
#else
    return 1;
#endif
}

void platform_shortcut_unregister(const char *shortcut) {
#if defined(_WIN32)
    UnregisterHotKey(NULL, 101);
#endif
}

int platform_deeplink_register(const char *scheme) {
    if (!scheme || scheme[0] == '\0') return 0;
    std::string s_scheme(scheme);
    if (s_scheme.size() > 64 || !std::isalpha(static_cast<unsigned char>(s_scheme[0]))) return 0;
    std::string lower_scheme;
    for (unsigned char c : s_scheme) {
        if (!std::isalnum(c) && c != '+' && c != '.' && c != '-') return 0;
        lower_scheme.push_back(static_cast<char>(std::tolower(c)));
    }
    static const char *blocked_schemes[] = {
        "http", "https", "file", "javascript", "data", "vbscript", "about",
        "exefile", "mscfile", "ms-settings", "cmdfile", "batfile", "comfile", "regfile", "htafile", "mailto"
    };
    for (const char *b : blocked_schemes) {
        if (lower_scheme == b) return 0;
    }
#if defined(_WIN32)
    std::vector<wchar_t> exe_path_w(32768, 0);
    DWORD exe_len = GetModuleFileNameW(NULL, exe_path_w.data(), static_cast<DWORD>(exe_path_w.size()));
    if (exe_len == 0 || exe_len >= exe_path_w.size()) return 0;
    std::wstring scheme_w(lower_scheme.begin(), lower_scheme.end());
    std::wstring key_w = L"Software\\Classes\\" + scheme_w;
    HKEY hKey = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key_w.c_str(), 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        std::wstring desc_w = L"URL:" + scheme_w + L" Protocol";
        RegSetValueExW(hKey, L"", 0, REG_SZ, (const BYTE *)desc_w.c_str(), static_cast<DWORD>((desc_w.size() + 1) * sizeof(wchar_t)));
        const wchar_t empty_proto[] = L"";
        RegSetValueExW(hKey, L"URL Protocol", 0, REG_SZ, (const BYTE *)empty_proto, sizeof(empty_proto));
        std::wstring cmd_w = L"\"" + std::wstring(exe_path_w.data(), exe_len) + L"\" \"%1\"";
        HKEY hCmd = NULL;
        int ok = 0;
        if (RegCreateKeyExW(hKey, L"shell\\open\\command", 0, NULL, 0, KEY_WRITE, NULL, &hCmd, NULL) == ERROR_SUCCESS) {
            if (RegSetValueExW(hCmd, L"", 0, REG_SZ, (const BYTE *)cmd_w.c_str(), static_cast<DWORD>((cmd_w.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS) {
                ok = 1;
            }
            RegCloseKey(hCmd);
        }
        RegCloseKey(hKey);
        return ok;
    }
    return 0;
#elif defined(__APPLE__)
    return 1;
#else
    return 1;
#endif
}

int platform_updater_install(const char *archive_path) {
    (void)archive_path;
    // Security remediation: Unauthenticated tar extraction from CWD is disabled
    return 0;
}

int platform_dialog_open(const char *title, const char *filter, char *out_path, int max_len) {
    if (!out_path || max_len <= 0) return 0;
    out_path[0] = '\0';
#if defined(_WIN32)
    OPENFILENAMEA ofn;
    char szFile[MAX_PATH] = {0};
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    std::string filter_buf;
    if (filter && filter[0] != '\0') {
        filter_buf.append("Files");
        filter_buf.push_back('\0');
        filter_buf.append(filter);
        filter_buf.push_back('\0');
        filter_buf.push_back('\0');
        ofn.lpstrFilter = filter_buf.data();
    } else {
        ofn.lpstrFilter = "All Files\0*.*\0\0";
    }
    ofn.nFilterIndex = 1;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) {
        strncpy(out_path, ofn.lpstrFile, static_cast<size_t>(max_len - 1));
        out_path[max_len - 1] = '\0';
        return 1;
    }
    return 0;
#elif defined(__APPLE__)
    __block int res = 0;
    std::string s_title = title ? title : "";
    run_on_main_sync(^{
        @try {
            NSOpenPanel *panel = [NSOpenPanel openPanel];
            [panel setCanChooseFiles:YES];
            [panel setCanChooseDirectories:NO];
            [panel setAllowsMultipleSelection:NO];
            if (!s_title.empty()) {
                NSString *nsTitle = webview_nsstring_utf8(s_title);
                if ([nsTitle length] > 0) {
                    [panel setMessage:nsTitle];
                }
            }
            if ([panel runModal] == NSModalResponseOK) {
                NSURL *url = [panel URL];
                if (!url) {
                    NSArray<NSURL *> *urls = [panel URLs];
                    if (urls && [urls count] > 0) {
                        url = [urls objectAtIndex:0];
                    }
                }
                if (url) {
                    const char *fs_rep = [url fileSystemRepresentation];
                    if (fs_rep) {
                        strncpy(out_path, fs_rep, static_cast<size_t>(max_len - 1));
                        out_path[max_len - 1] = '\0';
                        res = 1;
                    } else {
                        NSString *path = [url path];
                        if (path) {
                            const char *utf8 = [path UTF8String];
                            if (utf8) {
                                strncpy(out_path, utf8, static_cast<size_t>(max_len - 1));
                                out_path[max_len - 1] = '\0';
                                res = 1;
                            }
                        }
                    }
                }
            }
        } @catch (NSException *ex) {
            NSLog(@"[platform_dialog_open] Handled Exception: %@", ex);
            res = 0;
        }
    });
    return res;
#else
    GtkWidget *dialog = gtk_file_chooser_dialog_new(title ? title : "Open File", NULL, GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
    int res = 0;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (filename) {
            strncpy(out_path, filename, static_cast<size_t>(max_len - 1));
            out_path[max_len - 1] = '\0';
            g_free(filename);
            res = 1;
        }
    }
    gtk_widget_destroy(dialog);
    while (g_main_context_iteration(NULL, FALSE));
    return res;
#endif
}

int platform_dialog_save(const char *title, const char *filter, char *out_path, int max_len) {
    if (!out_path || max_len <= 0) return 0;
    out_path[0] = '\0';
#if defined(_WIN32)
    OPENFILENAMEA ofn;
    char szFile[MAX_PATH] = {0};
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    std::string filter_buf;
    if (filter && filter[0] != '\0') {
        filter_buf.append("Files");
        filter_buf.push_back('\0');
        filter_buf.append(filter);
        filter_buf.push_back('\0');
        filter_buf.push_back('\0');
        ofn.lpstrFilter = filter_buf.data();
    } else {
        ofn.lpstrFilter = "All Files\0*.*\0\0";
    }
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameA(&ofn)) {
        strncpy(out_path, ofn.lpstrFile, static_cast<size_t>(max_len - 1));
        out_path[max_len - 1] = '\0';
        return 1;
    }
    return 0;
#elif defined(__APPLE__)
    __block int res = 0;
    std::string s_title = title ? title : "";
    run_on_main_sync(^{
        @try {
            NSSavePanel *panel = [NSSavePanel savePanel];
            if (!s_title.empty()) {
                NSString *nsTitle = webview_nsstring_utf8(s_title);
                if ([nsTitle length] > 0) {
                    [panel setMessage:nsTitle];
                }
            }
            if ([panel runModal] == NSModalResponseOK) {
                NSURL *url = [panel URL];
                if (url) {
                    const char *fs_rep = [url fileSystemRepresentation];
                    if (fs_rep) {
                        strncpy(out_path, fs_rep, static_cast<size_t>(max_len - 1));
                        out_path[max_len - 1] = '\0';
                        res = 1;
                    } else {
                        NSString *path = [url path];
                        if (path) {
                            const char *utf8 = [path UTF8String];
                            if (utf8) {
                                strncpy(out_path, utf8, static_cast<size_t>(max_len - 1));
                                out_path[max_len - 1] = '\0';
                                res = 1;
                            }
                        }
                    }
                }
            }
        } @catch (NSException *ex) {
            NSLog(@"[platform_dialog_save] Handled Exception: %@", ex);
            res = 0;
        }
    });
    return res;
#else
    GtkWidget *dialog = gtk_file_chooser_dialog_new(title ? title : "Save File", NULL, GTK_FILE_CHOOSER_ACTION_SAVE, "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
    int res = 0;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (filename) {
            strncpy(out_path, filename, static_cast<size_t>(max_len - 1));
            out_path[max_len - 1] = '\0';
            g_free(filename);
            res = 1;
        }
    }
    gtk_widget_destroy(dialog);
    while (g_main_context_iteration(NULL, FALSE));
    return res;
#endif
}

int platform_dialog_message(const char *title, const char *message, int is_question) {
#if defined(_WIN32)
    UINT type = is_question ? (MB_YESNO | MB_ICONQUESTION) : (MB_OK | MB_ICONINFORMATION);
    int ret = MessageBoxA(NULL, message, title, type);
    return is_question ? (ret == IDYES ? 1 : 0) : 1;
#elif defined(__APPLE__)
    __block int ret = 1;
    std::string s_title = title ? title : "";
    std::string s_msg = message ? message : "";
    run_on_main_sync(^{
        NSAlert *alert = [[NSAlert alloc] init];
        [alert setMessageText:webview_nsstring_utf8(s_title)];
        [alert setInformativeText:webview_nsstring_utf8(s_msg)];
        if (is_question) {
            [alert addButtonWithTitle:@"Yes"];
            [alert addButtonWithTitle:@"No"];
        } else {
            [alert addButtonWithTitle:@"OK"];
        }
        NSModalResponse res = [alert runModal];
        ret = is_question ? (res == NSAlertFirstButtonReturn ? 1 : 0) : 1;
    });
    return ret;
#else
    GtkMessageType msg_type = is_question ? GTK_MESSAGE_QUESTION : GTK_MESSAGE_INFO;
    GtkButtonsType btn_type = is_question ? GTK_BUTTONS_YES_NO : GTK_BUTTONS_OK;
    GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, msg_type, btn_type, "%s", message);
    if (title) gtk_window_set_title(GTK_WINDOW(dialog), title);
    int res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    while (g_main_context_iteration(NULL, FALSE));
    return is_question ? (res == GTK_RESPONSE_YES ? 1 : 0) : 1;
#endif
}

int platform_clipboard_write(const char *text) {
#if defined(_WIN32)
    if (!OpenClipboard(NULL)) return 0;
    EmptyClipboard();
    size_t len = strlen(text) + 1;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
    if (!hMem) {
        CloseClipboard();
        return 0;
    }
    memcpy(GlobalLock(hMem), text, len);
    GlobalUnlock(hMem);
    SetClipboardData(CF_TEXT, hMem);
    CloseClipboard();
    return 1;
#elif defined(__APPLE__)
    std::string s_text = text ? text : "";
    run_on_main_sync(^{
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        [pb setString:webview_nsstring_utf8(s_text) forType:NSPasteboardTypeString];
    });
    return 1;
#else
    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(clipboard, text, -1);
    gtk_clipboard_store(clipboard);
    return 1;
#endif
}

int platform_clipboard_read(char *out_text, int max_len) {
    if (!out_text || max_len <= 0) return 0;
    out_text[0] = '\0';
#if defined(_WIN32)
    if (!OpenClipboard(NULL)) return 0;
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (hData) {
        SIZE_T gsize = GlobalSize(hData);
        const wchar_t *wstr = static_cast<const wchar_t *>(GlobalLock(hData));
        if (wstr && gsize >= sizeof(wchar_t)) {
            size_t max_wchars = gsize / sizeof(wchar_t);
            size_t wlen = wcsnlen(wstr, max_wchars);
            int ulen = WideCharToMultiByte(CP_UTF8, 0, wstr, static_cast<int>(wlen), NULL, 0, NULL, NULL);
            if (ulen > 0) {
                std::string u8(static_cast<size_t>(ulen), '\0');
                WideCharToMultiByte(CP_UTF8, 0, wstr, static_cast<int>(wlen), &u8[0], ulen, NULL, NULL);
                size_t copy_len = std::min(static_cast<size_t>(max_len - 1), u8.size());
                memcpy(out_text, u8.data(), copy_len);
                out_text[copy_len] = '\0';
            }
            GlobalUnlock(hData);
            CloseClipboard();
            return 1;
        }
        if (wstr) GlobalUnlock(hData);
    }
    HANDLE hAnsi = GetClipboardData(CF_TEXT);
    if (hAnsi) {
        SIZE_T gsize = GlobalSize(hAnsi);
        const char *pszText = static_cast<const char *>(GlobalLock(hAnsi));
        if (pszText && gsize > 0) {
            size_t slen = strnlen(pszText, gsize);
            size_t copy_len = std::min(static_cast<size_t>(max_len - 1), slen);
            memcpy(out_text, pszText, copy_len);
            out_text[copy_len] = '\0';
            GlobalUnlock(hAnsi);
            CloseClipboard();
            return 1;
        }
        if (pszText) GlobalUnlock(hAnsi);
    }
    CloseClipboard();
    return 0;
#elif defined(__APPLE__)
    __block int ok = 0;
    run_on_main_sync(^{
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        NSString *str = [pb stringForType:NSPasteboardTypeString];
        if (str) {
            const char *utf8 = [str UTF8String];
            if (utf8) {
                strncpy(out_text, utf8, static_cast<size_t>(max_len - 1));
                out_text[max_len - 1] = '\0';
                ok = 1;
            }
        }
    });
    return ok;
#else
    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gchar *text = gtk_clipboard_wait_for_text(clipboard);
    if (text) {
        strncpy(out_text, text, static_cast<size_t>(max_len - 1));
        out_text[max_len - 1] = '\0';
        g_free(text);
        return 1;
    }
    return 0;
#endif
}

static const std::string b64_table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64_encode(const unsigned char *data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    int val = 0, valb = -6;
    for (size_t i = 0; i < len; ++i) {
        val = (val << 8) + data[i];
        valb += 8;
        while (valb >= 0) {
            out.push_back(b64_table[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(b64_table[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

static std::vector<unsigned char> base64_decode(const std::string &in) {
    std::vector<unsigned char> out;
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; ++i) T[(unsigned char)b64_table[i]] = i;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back((unsigned char)((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

int platform_clipboard_write_html(const char *html) {
    if (!html) return 0;
#if defined(__APPLE__)
    std::string s_html = html;
    run_on_main_sync(^{
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        [pb setString:webview_nsstring_utf8(s_html) forType:NSPasteboardTypeHTML];
    });
    return 1;
#elif defined(_WIN32)
    UINT cfHtml = RegisterClipboardFormatA("HTML Format");
    if (!OpenClipboard(NULL)) return 0;
    EmptyClipboard();
    size_t len = strlen(html) + 1;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
    if (!hMem) { CloseClipboard(); return 0; }
    memcpy(GlobalLock(hMem), html, len);
    GlobalUnlock(hMem);
    SetClipboardData(cfHtml, hMem);
    CloseClipboard();
    return 1;
#else
    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(clipboard, html, -1);
    gtk_clipboard_store(clipboard);
    return 1;
#endif
}

int platform_clipboard_read_html(char *out_html, int max_len) {
    if (!out_html || max_len <= 0) return 0;
#if defined(__APPLE__)
    __block int ok = 0;
    run_on_main_sync(^{
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        NSString *str = [pb stringForType:NSPasteboardTypeHTML];
        if (str) {
            const char *utf8 = [str UTF8String];
            if (utf8) {
                strncpy(out_html, utf8, static_cast<size_t>(max_len - 1));
                out_html[max_len - 1] = '\0';
                ok = 1;
            }
        }
    });
    return ok;
#elif defined(_WIN32)
    UINT cfHtml = RegisterClipboardFormatA("HTML Format");
    if (!OpenClipboard(NULL)) return 0;
    HANDLE hData = GetClipboardData(cfHtml);
    if (!hData) { CloseClipboard(); return 0; }
    char *psz = (char *)GlobalLock(hData);
    if (psz) {
        strncpy(out_html, psz, max_len - 1);
        out_html[max_len - 1] = '\0';
        GlobalUnlock(hData);
        CloseClipboard();
        return 1;
    }
    CloseClipboard();
    return 0;
#else
    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gchar *text = gtk_clipboard_wait_for_text(clipboard);
    if (text) {
        strncpy(out_html, text, max_len - 1);
        out_html[max_len - 1] = '\0';
        g_free(text);
        return 1;
    }
    return 0;
#endif
}

int platform_clipboard_write_image(const char *base64_png) {
    if (!base64_png) return 0;
    std::string b64 = base64_png;
    size_t comma = b64.find(',');
    if (comma != std::string::npos && b64.find("data:image/") != std::string::npos) {
        b64 = b64.substr(comma + 1);
    }
    std::vector<unsigned char> bytes = base64_decode(b64);
    if (bytes.empty()) return 0;

#if defined(__APPLE__)
    run_on_main_sync(^{
        NSData *nsdata = [NSData dataWithBytes:bytes.data() length:bytes.size()];
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        [pb setData:nsdata forType:NSPasteboardTypePNG];
    });
    return 1;
#else
    return 1;
#endif
}

int platform_clipboard_read_image(char *out_base64_png, int max_len) {
    if (!out_base64_png || max_len <= 0) return 0;
#if defined(__APPLE__)
    __block int ok = 0;
    run_on_main_sync(^{
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        NSData *data = [pb dataForType:NSPasteboardTypePNG];
        if (!data) {
            data = [pb dataForType:NSPasteboardTypeTIFF];
            if (data) {
                NSBitmapImageRep *rep = [NSBitmapImageRep imageRepWithData:data];
                data = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
            }
        }
        if (data && [data length] > 0) {
            std::string encoded = base64_encode(static_cast<const unsigned char *>([data bytes]), [data length]);
            strncpy(out_base64_png, encoded.c_str(), max_len - 1);
            out_base64_png[max_len - 1] = '\0';
            ok = 1;
        }
    });
    return ok;
#else
    return 0;
#endif
}

void platform_clipboard_clear() {
#if defined(__APPLE__)
    run_on_main_sync(^{
        [[NSPasteboard generalPasteboard] clearContents];
    });
#elif defined(_WIN32)
    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        CloseClipboard();
    }
#else
    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_clear(clipboard);
#endif
}

static theme_callback_fn g_theme_cb = nullptr;
static bool g_theme_observer_installed = false;

const char *platform_get_theme() {
#if defined(__APPLE__)
    NSString *appleInterfaceStyle = [[NSUserDefaults standardUserDefaults] stringForKey:@"AppleInterfaceStyle"];
    if (appleInterfaceStyle && [appleInterfaceStyle isEqualToString:@"Dark"]) {
        return "dark";
    }
    if (@available(macOS 10.14, *)) {
        NSAppearance *app = [NSApp effectiveAppearance];
        NSAppearanceName bestMatch = [app bestMatchFromAppearancesWithNames:@[NSAppearanceNameAqua, NSAppearanceNameDarkAqua]];
        if ([bestMatch isEqualToString:NSAppearanceNameDarkAqua]) {
            return "dark";
        }
    }
    return "light";
#elif defined(_WIN32)
    DWORD val = 1;
    DWORD size = sizeof(val);
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", "AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &val, &size) == ERROR_SUCCESS) {
        return val == 0 ? "dark" : "light";
    }
    return "light";
#else
    GtkSettings *settings = gtk_settings_get_default();
    if (settings) {
        gchar *theme_name = NULL;
        gboolean prefer_dark = FALSE;
        g_object_get(settings, "gtk-theme-name", &theme_name, "gtk-application-prefer-dark-theme", &prefer_dark, NULL);
        bool is_dark = prefer_dark;
        if (theme_name) {
            if (strcasestr(theme_name, "dark")) is_dark = true;
            g_free(theme_name);
        }
        if (is_dark) return "dark";
    }
    return "light";
#endif
}

void platform_theme_set_callback(theme_callback_fn cb) {
    g_theme_cb = cb;
#if defined(__APPLE__)
    if (!g_theme_observer_installed) {
        g_theme_observer_installed = true;
        [[NSDistributedNotificationCenter defaultCenter] addObserverForName:@"AppleInterfaceThemeChangedNotification"
                                                                     object:nil
                                                                      queue:[NSOperationQueue mainQueue]
                                                                 usingBlock:^(NSNotification *note) {
            if (g_theme_cb) {
                g_theme_cb(platform_get_theme());
            }
        }];
    }
#elif !defined(_WIN32)
    GtkSettings *settings = gtk_settings_get_default();
    if (settings && !g_theme_observer_installed) {
        g_theme_observer_installed = true;
        g_signal_connect(settings, "notify::gtk-theme-name", G_CALLBACK(+[](GObject *, GParamSpec *, gpointer) {
            if (g_theme_cb) g_theme_cb(platform_get_theme());
        }), NULL);
        g_signal_connect(settings, "notify::gtk-application-prefer-dark-theme", G_CALLBACK(+[](GObject *, GParamSpec *, gpointer) {
            if (g_theme_cb) g_theme_cb(platform_get_theme());
        }), NULL);
    }
#endif
}

#if defined(__APPLE__)
@interface ModernNotificationCenterDelegate : NSObject <UNUserNotificationCenterDelegate>
@end

@implementation ModernNotificationCenterDelegate
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
       willPresentNotification:(UNNotification *)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions options))completionHandler {
    if (@available(macOS 11.0, *)) {
        completionHandler(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionList | UNNotificationPresentationOptionSound);
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        completionHandler(UNNotificationPresentationOptionAlert | UNNotificationPresentationOptionSound);
#pragma clang diagnostic pop
    }
}
@end
#endif

static std::string truncate_utf8_safe(const std::string &s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    size_t len = max_bytes;
    while (len > 0 && (static_cast<unsigned char>(s[len]) & 0xC0) == 0x80) {
        --len;
    }
    return s.substr(0, len);
}

void platform_notification_send(const char *title, const char *body) {
    if (!title && !body) return;
    std::string s_title = title ? title : "";
    std::string s_body = body ? body : "";

    s_title.erase(std::remove(s_title.begin(), s_title.end(), '\0'), s_title.end());
    s_body.erase(std::remove(s_body.begin(), s_body.end(), '\0'), s_body.end());
    s_title = truncate_utf8_safe(s_title, 256);
    s_body = truncate_utf8_safe(s_body, 1024);

#if defined(__APPLE__)
    __block bool delivered = false;
    if ([NSBundle mainBundle].bundleIdentifier != nil) {
        run_on_main_sync(^{
            if (@available(macOS 10.14, *)) {
                static ModernNotificationCenterDelegate *s_delegate = nil;
                static dispatch_once_t onceToken;
                dispatch_once(&onceToken, ^{
                    s_delegate = [[ModernNotificationCenterDelegate alloc] init];
                    [[UNUserNotificationCenter currentNotificationCenter] setDelegate:s_delegate];
                    [[UNUserNotificationCenter currentNotificationCenter] requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound)
                                                                                        completionHandler:^(BOOL granted, NSError * _Nullable error) {}];
                });

                UNMutableNotificationContent *content = [[UNMutableNotificationContent alloc] init];
                content.title = webview_nsstring_utf8(s_title);
                content.body = webview_nsstring_utf8(s_body);
                content.sound = [UNNotificationSound defaultSound];

                NSString *identifier = [[NSUUID UUID] UUIDString];
                UNNotificationRequest *request = [UNNotificationRequest requestWithIdentifier:identifier content:content trigger:nil];
                [[UNUserNotificationCenter currentNotificationCenter] addNotificationRequest:request withCompletionHandler:nil];
                delivered = true;
            }
        });
    }
    if (delivered) return;

    std::thread([s_title, s_body]() {
        static const char *fixed_script = "on run argv\n  display notification (item 2 of argv) with title (item 1 of argv)\nend run";
        pid_t pid = -1;
        const char *argv[] = {"osascript", "-e", fixed_script, "--", s_title.c_str(), s_body.c_str(), NULL};
        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
        int rc = posix_spawn(&pid, "/usr/bin/osascript", NULL, &attr, const_cast<char *const *>(argv), environ);
        posix_spawnattr_destroy(&attr);
        if (rc == 0 && pid > 0) {
            int status = 0;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        }
    }).detach();

#elif defined(_WIN32)
    std::thread([s_title, s_body]() {
        NOTIFYICONDATAW nid;
        ZeroMemory(&nid, sizeof(nid));
        nid.cbSize = sizeof(nid);
        HWND hwnd = NULL;
        {
            std::lock_guard<std::mutex> lock(g_windows_mutex);
            if (!g_windows.empty()) {
                hwnd = static_cast<HWND>(webview_get_window(g_windows.begin()->second.w));
            }
        }
        nid.hWnd = hwnd;
        nid.uID = 1001;
        nid.uFlags = NIF_INFO | NIF_MESSAGE;
        nid.dwInfoFlags = NIIF_INFO;

        MultiByteToWideChar(CP_UTF8, 0, s_title.c_str(), -1, nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle));
        MultiByteToWideChar(CP_UTF8, 0, s_body.c_str(), -1, nid.szInfo, ARRAYSIZE(nid.szInfo));

        Shell_NotifyIconW(NIM_ADD, &nid);
        Sleep(4000);
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }).detach();

#else
    std::thread([s_title, s_body]() {
        pid_t pid = -1;
        const char *argv[] = {"notify-send", "--", s_title.c_str(), s_body.c_str(), NULL};
        if (posix_spawnp(&pid, "notify-send", NULL, NULL, const_cast<char *const *>(argv), environ) == 0 && pid > 0) {
            int status = 0;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        }
    }).detach();
#endif
}

static single_instance_callback_fn g_single_instance_cb = nullptr;
static std::thread g_single_instance_thread;
static std::atomic<bool> g_single_instance_running{false};
static std::atomic<bool> g_single_instance_initialized{false};
static std::string g_single_instance_sock_path;
#if !defined(_WIN32)
static int g_single_instance_server_fd = -1;
#else
static HANDLE g_single_instance_pipe = INVALID_HANDLE_VALUE;
#endif

void platform_single_instance_set_callback(single_instance_callback_fn cb) {
    g_single_instance_cb = cb;
}

void platform_single_instance_cleanup() {
    g_single_instance_running = false;
#if !defined(_WIN32)
    if (g_single_instance_server_fd >= 0) {
        shutdown(g_single_instance_server_fd, SHUT_RDWR);
        close(g_single_instance_server_fd);
        g_single_instance_server_fd = -1;
    }
    if (g_single_instance_thread.joinable()) {
        g_single_instance_thread.join();
    }
    if (!g_single_instance_sock_path.empty()) {
        unlink(g_single_instance_sock_path.c_str());
        g_single_instance_sock_path.clear();
    }
#else
    if (g_single_instance_pipe != INVALID_HANDLE_VALUE) {
        CancelIoEx(g_single_instance_pipe, NULL);
        CloseHandle(g_single_instance_pipe);
        g_single_instance_pipe = INVALID_HANDLE_VALUE;
    }
    if (g_single_instance_thread.joinable()) {
        g_single_instance_thread.detach();
    }
#endif
    g_single_instance_initialized = false;
}

int platform_single_instance_init(const char *app_id, const char *payload_json) {
    if (g_single_instance_initialized.load()) return 1;

    std::string safe_id = (app_id && strlen(app_id) > 0) ? app_id : "com.tauri.lua";
    for (char &c : safe_id) {
        if (!isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '.' && c != '_') {
            c = '_';
        }
    }
    std::string payload = (payload_json && strlen(payload_json) > 0) ? payload_json : "{}";

#if !defined(_WIN32)
    std::string sock_path;
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir && strlen(runtime_dir) > 0) {
        sock_path = std::string(runtime_dir) + "/" + safe_id + ".sock";
    } else {
        const char *home = getenv("HOME");
        if (home && strlen(home) > 0) {
            std::string local_dir = std::string(home) + "/.local";
            std::string run_dir = local_dir + "/run";
            ::mkdir(local_dir.c_str(), 0700);
            ::mkdir(run_dir.c_str(), 0700);
            struct stat st;
            if (lstat(run_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid()) {
                return -1;
            }
            ::chmod(run_dir.c_str(), 0700);
            sock_path = run_dir + "/" + safe_id + ".sock";
        } else {
            return -1;
        }
    }

    int client_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client_sock >= 0) {
#if defined(FD_CLOEXEC)
        fcntl(client_sock, F_SETFD, FD_CLOEXEC);
#endif
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        if (sock_path.size() < sizeof(addr.sun_path)) {
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);
            addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';

            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 300000;
            setsockopt(client_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            if (connect(client_sock, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0) {
                std::string msg = payload + "\n";
                send(client_sock, msg.c_str(), msg.length(), 0);
                close(client_sock);
                return 0;
            }
        }
        close(client_sock);
    }

    {
        struct stat st;
        if (lstat(sock_path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode)) {
            unlink(sock_path.c_str());
        }
    }

    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) return -1;
#if defined(FD_CLOEXEC)
    fcntl(server_fd, F_SETFD, FD_CLOEXEC);
#endif

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_un saddr;
    memset(&saddr, 0, sizeof(saddr));
    if (sock_path.size() >= sizeof(saddr.sun_path)) {
        close(server_fd);
        return -1;
    }
    saddr.sun_family = AF_UNIX;
    strncpy(saddr.sun_path, sock_path.c_str(), sizeof(saddr.sun_path) - 1);
    saddr.sun_path[sizeof(saddr.sun_path) - 1] = '\0';

    if (bind(server_fd, reinterpret_cast<struct sockaddr *>(&saddr), sizeof(saddr)) < 0) {
        close(server_fd);
        return -1;
    }

    if (chmod(sock_path.c_str(), 0600) != 0) {
        close(server_fd);
        unlink(sock_path.c_str());
        return -1;
    }

    if (listen(server_fd, 5) < 0) {
        close(server_fd);
        unlink(sock_path.c_str());
        return -1;
    }

    g_single_instance_sock_path = sock_path;
    g_single_instance_server_fd = server_fd;
    g_single_instance_running = true;
    g_single_instance_initialized = true;

    g_single_instance_thread = std::thread([server_fd]() {
        const uid_t owner_uid = geteuid();
        while (g_single_instance_running.load()) {
            struct pollfd pfd;
            pfd.fd = server_fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int pr = poll(&pfd, 1, 250);
            if (pr <= 0) {
                if (!g_single_instance_running.load()) break;
                continue;
            }
            if (!(pfd.revents & POLLIN)) {
                if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
                continue;
            }
            struct sockaddr_un client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(server_fd, reinterpret_cast<struct sockaddr *>(&client_addr), &client_len);
            if (client_fd < 0) {
                if (!g_single_instance_running.load()) break;
                continue;
            }
#if defined(FD_CLOEXEC)
            fcntl(client_fd, F_SETFD, FD_CLOEXEC);
#endif
            struct timeval tv;
            tv.tv_sec = 2;
            tv.tv_usec = 0;
            setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

#if defined(__APPLE__)
            uid_t peer_uid;
            gid_t peer_gid;
            if (getpeereid(client_fd, &peer_uid, &peer_gid) != 0 || peer_uid != owner_uid) {
                close(client_fd);
                continue;
            }
#elif defined(SO_PEERCRED)
            struct ucred peer_cred;
            socklen_t cred_len = sizeof(peer_cred);
            if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &peer_cred, &cred_len) != 0 ||
                cred_len != sizeof(peer_cred) || peer_cred.uid != owner_uid) {
                close(client_fd);
                continue;
            }
#else
            close(client_fd);
            continue;
#endif

            std::string buffer;
            char chunk[512];
            ssize_t bytes_read = 0;
            size_t total_read = 0;
            bool oversized = false;
            const size_t MAX_MSG_BYTES = 65536;
            while ((bytes_read = recv(client_fd, chunk, sizeof(chunk), 0)) > 0) {
                total_read += static_cast<size_t>(bytes_read);
                if (total_read > MAX_MSG_BYTES) {
                    oversized = true;
                    break;
                }
                buffer.append(chunk, static_cast<size_t>(bytes_read));
                if (buffer.find('\n') != std::string::npos) break;
            }
            close(client_fd);
            if (oversized || buffer.find('\0') != std::string::npos) {
                continue;
            }

            while (!buffer.empty() && (buffer.back() == '\n' || buffer.back() == '\r')) {
                buffer.pop_back();
            }
            if (buffer.empty()) buffer = "{}";

            platform_window_set_focus("main");

            if (g_single_instance_cb) {
                g_single_instance_cb(buffer.c_str());
            }
        }
    });

    return 1;

#else
    std::wstring sid_suffix;
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        DWORD needed = 0;
        GetTokenInformation(hToken, TokenUser, NULL, 0, &needed);
        if (needed > 0) {
            std::vector<BYTE> tok_buf(needed);
            if (GetTokenInformation(hToken, TokenUser, tok_buf.data(), needed, &needed)) {
                TOKEN_USER *tu = reinterpret_cast<TOKEN_USER *>(tok_buf.data());
                LPWSTR sid_str = NULL;
                if (ConvertSidToStringSidW(tu->User.Sid, &sid_str) && sid_str) {
                    sid_suffix = L"-" + std::wstring(sid_str);
                    LocalFree(sid_str);
                }
            }
        }
        CloseHandle(hToken);
    }

    std::wstring safe_id_w(safe_id.begin(), safe_id.end());
    std::wstring pipe_name_w = L"\\\\.\\pipe\\" + safe_id_w + sid_suffix + L"-single-instance";

    HANDLE hPipe = CreateFileW(pipe_name_w.c_str(), GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hPipe != INVALID_HANDLE_VALUE) {
        std::string msg = payload + "\n";
        DWORD bytesWritten = 0;
        WriteFile(hPipe, msg.c_str(), static_cast<DWORD>(msg.length()), &bytesWritten, NULL);
        CloseHandle(hPipe);
        return 0;
    }

    auto create_secure_pipe = [&](bool first_instance) -> HANDLE {
        SECURITY_ATTRIBUTES sa;
        ZeroMemory(&sa, sizeof(sa));
        sa.nLength = sizeof(sa);
        PSECURITY_DESCRIPTOR pSD = NULL;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;OW)", SDDL_REVISION_1, &pSD, NULL)) {
            return INVALID_HANDLE_VALUE;
        }
        sa.lpSecurityDescriptor = pSD;
        DWORD open_mode = PIPE_ACCESS_DUPLEX | (first_instance ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
        HANDLE hp = CreateNamedPipeW(
            pipe_name_w.c_str(),
            open_mode,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES,
            4096, 4096, 2000, &sa
        );
        LocalFree(pSD);
        return hp;
    };

    HANDLE first_pipe = create_secure_pipe(true);
    if (first_pipe == INVALID_HANDLE_VALUE) return -1;

    g_single_instance_pipe = first_pipe;
    g_single_instance_running = true;
    g_single_instance_initialized = true;

    g_single_instance_thread = std::thread([pipe_name_w, first_pipe]() {
        HANDLE current_pipe = first_pipe;
        while (g_single_instance_running.load()) {
            if (current_pipe == INVALID_HANDLE_VALUE) {
                SECURITY_ATTRIBUTES sa;
                ZeroMemory(&sa, sizeof(sa));
                sa.nLength = sizeof(sa);
                PSECURITY_DESCRIPTOR pSD = NULL;
                if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;OW)", SDDL_REVISION_1, &pSD, NULL)) {
                    break;
                }
                sa.lpSecurityDescriptor = pSD;
                current_pipe = CreateNamedPipeW(
                    pipe_name_w.c_str(),
                    PIPE_ACCESS_DUPLEX,
                    PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                    PIPE_UNLIMITED_INSTANCES,
                    4096, 4096, 2000, &sa
                );
                LocalFree(pSD);
                if (current_pipe == INVALID_HANDLE_VALUE) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    continue;
                }
            }
            BOOL connected = ConnectNamedPipe(current_pipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
            if (connected && g_single_instance_running.load()) {
                char buf[4096];
                DWORD bytesRead = 0;
                std::string buffer;
                bool oversized = false;
                const size_t MAX_MSG_BYTES = 65536;
                while (ReadFile(current_pipe, buf, sizeof(buf), &bytesRead, NULL) && bytesRead > 0) {
                    if (buffer.size() + bytesRead > MAX_MSG_BYTES) {
                        oversized = true;
                        break;
                    }
                    buffer.append(buf, bytesRead);
                    if (buffer.find('\n') != std::string::npos) break;
                }
                DisconnectNamedPipe(current_pipe);
                CloseHandle(current_pipe);
                current_pipe = INVALID_HANDLE_VALUE;
                if (oversized || buffer.find('\0') != std::string::npos) {
                    continue;
                }

                while (!buffer.empty() && (buffer.back() == '\n' || buffer.back() == '\r')) {
                    buffer.pop_back();
                }
                if (buffer.empty()) buffer = "{}";

                platform_window_set_focus("main");
                if (g_single_instance_cb) {
                    g_single_instance_cb(buffer.c_str());
                }
            } else {
                CloseHandle(current_pipe);
                current_pipe = INVALID_HANDLE_VALUE;
            }
        }
        if (current_pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(current_pipe);
        }
    });

    return 1;
#endif
}

void platform_state_set_string(const char *key, const char *val) {
    if (!key || !val) return;
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    g_shared_state[key] = StateValue{StateType::String, val, 0.0, false};
    g_state_versions[key] = g_state_versions[key] + 1;
}

int platform_state_get_string(const char *key, char *out_val, int max_len) {
    if (!key) return 0;
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    auto it = g_shared_state.find(key);
    if (it == g_shared_state.end() || it->second.type != StateType::String) return 0;
    const std::string &s = it->second.str_data;
    int len = static_cast<int>(s.size());
    if (!out_val || max_len <= 0) return len + 1;
    if (len >= max_len) {
        std::memcpy(out_val, s.data(), static_cast<size_t>(max_len - 1));
        out_val[max_len - 1] = '\0';
        return len + 1;
    }
    std::memcpy(out_val, s.data(), static_cast<size_t>(len));
    out_val[len] = '\0';
    return 1;
}

void platform_state_set_number(const char *key, double val) {
    if (!key) return;
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    g_shared_state[key] = StateValue{StateType::Number, "", val, false};
    g_state_versions[key] = g_state_versions[key] + 1;
}

int platform_state_get_number(const char *key, double *out_val) {
    if (!key || !out_val) return 0;
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    auto it = g_shared_state.find(key);
    if (it != g_shared_state.end() && it->second.type == StateType::Number) {
        *out_val = it->second.num_data;
        return 1;
    }
    return 0;
}

void platform_state_set_bool(const char *key, int val) {
    if (!key) return;
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    g_shared_state[key] = StateValue{StateType::Bool, "", 0.0, val != 0};
    g_state_versions[key] = g_state_versions[key] + 1;
}

int platform_state_get_bool(const char *key, int *out_val) {
    if (!key || !out_val) return 0;
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    auto it = g_shared_state.find(key);
    if (it != g_shared_state.end() && it->second.type == StateType::Bool) {
        *out_val = it->second.bool_data ? 1 : 0;
        return 1;
    }
    return 0;
}

void platform_state_set_json(const char *key, const char *json_val) {
    if (!key || !json_val) return;
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    g_shared_state[key] = StateValue{StateType::Json, json_val, 0.0, false};
    g_state_versions[key] = g_state_versions[key] + 1;
}

int platform_state_get_json(const char *key, char *out_val, int max_len) {
    if (!key) return 0;
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    auto it = g_shared_state.find(key);
    if (it == g_shared_state.end() || it->second.type != StateType::Json) return 0;
    const std::string &s = it->second.str_data;
    int len = static_cast<int>(s.size());
    if (!out_val || max_len <= 0) return len + 1;
    if (len >= max_len) {
        std::memcpy(out_val, s.data(), static_cast<size_t>(max_len - 1));
        out_val[max_len - 1] = '\0';
        return len + 1;
    }
    std::memcpy(out_val, s.data(), static_cast<size_t>(len));
    out_val[len] = '\0';
    return 1;
}

uint64_t platform_state_get_version(const char *key) {
    if (!key) return g_state_epoch;
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    auto it = g_state_versions.find(key);
    uint64_t local_v = (it != g_state_versions.end()) ? it->second : 0;
    return (g_state_epoch << 32) ^ local_v;
}

int platform_state_has(const char *key) {
    if (!key) return 0;
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    return g_shared_state.find(key) != g_shared_state.end() ? 1 : 0;
}

void platform_state_delete(const char *key) {
    if (!key) return;
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    g_shared_state.erase(key);
    g_state_versions[key] = g_state_versions[key] + 1;
}

void platform_state_clear() {
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    g_shared_state.clear();
    g_state_versions.clear();
    g_state_epoch++;
}

int platform_state_get_length(const char *key) {
    if (!key) return 0;
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    auto it = g_shared_state.find(key);
    if (it != g_shared_state.end()) {
        if (it->second.type == StateType::String || it->second.type == StateType::Json) {
            return static_cast<int>(it->second.str_data.size());
        }
    }
    return 0;
}

void platform_state_lock() {
    g_state_mutex.lock();
}

void platform_state_unlock() {
    g_state_mutex.unlock();
}

void platform_set_csp(const char *csp) {
    if (csp && strlen(csp) > 0) {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        g_csp_policy_custom = csp;
    }
}

struct ProcessHandle {
#if defined(_WIN32)
    HANDLE hProcess = NULL;
    HANDLE hThread = NULL;
    HANDLE hRead = NULL;
    std::string line_buffer;
#else
    pid_t pid = -1;
    int pipe_fd = -1;
    std::string line_buffer;
    bool exited = false;
    int exit_code = 0;
#endif
};

void *platform_process_spawn(const char *program, const char **args, int arg_count) {
    if (!program || strlen(program) == 0) return nullptr;

#if defined(_WIN32)
    std::string prog_str = program;
    std::string prog_lower = prog_str;
    for (char &c : prog_lower) c = ::tolower((unsigned char)c);
    if (prog_lower.size() >= 4 && (prog_lower.substr(prog_lower.size() - 4) == ".bat" || prog_lower.substr(prog_lower.size() - 4) == ".cmd")) {
        ::fprintf(stderr, "[tauri-lua] error: direct execution of .bat / .cmd files is blocked for security (CWE-78)\n");
        return nullptr;
    }

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE hRead = NULL;
    HANDLE hWrite = NULL;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        return nullptr;
    }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    // Resolve executable path without CWD search
    int prog_wlen = MultiByteToWideChar(CP_UTF8, 0, prog_str.data(), (int)prog_str.size(), NULL, 0);
    std::wstring prog_w(prog_wlen, 0);
    if (prog_wlen > 0) {
        MultiByteToWideChar(CP_UTF8, 0, prog_str.data(), (int)prog_str.size(), &prog_w[0], prog_wlen);
    }

    // SearchPathW with explicit PATH avoids untrusted CWD resolution
    typedef BOOL (WINAPI *SetNeedCurrentDirFn)(BOOL);
    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    if (hK32) {
        SetNeedCurrentDirFn pfn = (SetNeedCurrentDirFn)GetProcAddress(hK32, "SetNeedCurrentDirectoryForExePathW");
        if (pfn) pfn(FALSE);
    }

    wchar_t abs_path_w[MAX_PATH];
    wchar_t path_env[32768];
    DWORD path_len = GetEnvironmentVariableW(L"PATH", path_env, 32768);
    LPCWSTR search_path = (path_len > 0 && path_len < 32768) ? path_env : L"";
    DWORD found = SearchPathW(search_path, prog_w.c_str(), L".exe", MAX_PATH, abs_path_w, NULL);
    if (found == 0 || found >= MAX_PATH) {
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return nullptr;
    }
    std::wstring resolved_lower(abs_path_w, found);
    while (!resolved_lower.empty() && (resolved_lower.back() == L'.' || resolved_lower.back() == L' ')) {
        resolved_lower.pop_back();
    }
    for (wchar_t &wc : resolved_lower) wc = static_cast<wchar_t>(towlower(wc));
    if (resolved_lower.find(L':', 2) != std::wstring::npos) {
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return nullptr;
    }
    auto has_blocked_ext = [&](const wchar_t *ext) -> bool {
        size_t elen = wcslen(ext);
        return resolved_lower.size() >= elen && resolved_lower.compare(resolved_lower.size() - elen, elen, ext) == 0;
    };
    if (has_blocked_ext(L".bat") || has_blocked_ext(L".cmd") || has_blocked_ext(L".com") ||
        has_blocked_ext(L".vbs") || has_blocked_ext(L".vbe") || has_blocked_ext(L".js") ||
        has_blocked_ext(L".jse") || has_blocked_ext(L".ps1") || has_blocked_ext(L".hta") ||
        has_blocked_ext(L".scr") || has_blocked_ext(L".pif") || has_blocked_ext(L".wsf")) {
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return nullptr;
    }
    LPCWSTR app_name_ptr = abs_path_w;

    std::wstring cmd_line_w;
    auto append_quoted_w = [&](const std::string &arg) {
        if (!cmd_line_w.empty()) cmd_line_w += L" ";
        int wlen = MultiByteToWideChar(CP_UTF8, 0, arg.data(), (int)arg.size(), NULL, 0);
        std::wstring warg(wlen, 0);
        if (wlen > 0) {
            MultiByteToWideChar(CP_UTF8, 0, arg.data(), (int)arg.size(), &warg[0], wlen);
        }
        bool needs_quotes = warg.empty() || (warg.find_first_of(L" \t\n\v\"") != std::wstring::npos);
        if (!needs_quotes) {
            cmd_line_w += warg;
        } else {
            cmd_line_w += L"\"";
            size_t backslashes = 0;
            for (wchar_t c : warg) {
                if (c == L'\\') {
                    backslashes++;
                } else if (c == L'\"') {
                    cmd_line_w.append(backslashes * 2 + 1, L'\\');
                    cmd_line_w += L'\"';
                    backslashes = 0;
                } else {
                    cmd_line_w.append(backslashes, L'\\');
                    backslashes = 0;
                    cmd_line_w += c;
                }
            }
            cmd_line_w.append(backslashes * 2, L'\\');
            cmd_line_w += L"\"";
        }
    };

    append_quoted_w(program);
    for (int i = 0; i < arg_count; ++i) {
        if (args && args[i]) {
            append_quoted_w(args[i]);
        }
    }

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdError = hWrite;
    si.hStdOutput = hWrite;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    std::vector<wchar_t> cmd_buf(cmd_line_w.begin(), cmd_line_w.end());
    cmd_buf.push_back(L'\0');

    BOOL success = CreateProcessW(
        app_name_ptr,
        cmd_buf.data(),
        NULL,
        NULL,
        TRUE,
        CREATE_NO_WINDOW,
        NULL,
        NULL,
        &si,
        &pi
    );

    CloseHandle(hWrite);

    if (!success) {
        CloseHandle(hRead);
        return nullptr;
    }

    ProcessHandle *proc = new ProcessHandle();
    proc->hProcess = pi.hProcess;
    proc->hThread = pi.hThread;
    proc->hRead = hRead;
    return proc;
#else
    int pipe_fds[2];
#if defined(__linux__) && defined(O_CLOEXEC)
    if (pipe2(pipe_fds, O_CLOEXEC) != 0) {
        return nullptr;
    }
#else
    if (pipe(pipe_fds) != 0) {
        return nullptr;
    }
#if defined(FD_CLOEXEC)
    fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC);
#endif
#endif

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);

    posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);

    std::vector<char *> argv;
    argv.push_back(const_cast<char *>(program));
    for (int i = 0; i < arg_count; ++i) {
        if (args && args[i]) {
            argv.push_back(const_cast<char *>(args[i]));
        }
    }
    argv.push_back(nullptr);

    std::vector<std::string> clean_env_storage;
    std::vector<char *> clean_envp;
    auto is_dangerous_env = [](const std::string &key) -> bool {
        if (key.rfind("LD_", 0) == 0 || key.rfind("DYLD_", 0) == 0) return true;
        static const char *blocked[] = {
            "NODE_OPTIONS", "PYTHONPATH", "PYTHONSTARTUP", "BASH_ENV", "ENV", "IFS",
            "GIT_SSH_COMMAND", "GIT_EXEC_PATH", "PERL5OPT", "PERL5LIB", "RUBYOPT",
            "JAVA_TOOL_OPTIONS", "_JAVA_OPTIONS", "GCONV_PATH", "HOSTALIASES",
            "MALLOC_TRACE", "SSLKEYLOGFILE", "SHELLOPTS", "PS4", "ZDOTDIR",
            "DOTNET_STARTUP_HOOKS", "CDPATH"
        };
        for (const char *b : blocked) {
            if (key == b) return true;
        }
        return false;
    };
    if (environ) {
        for (char **e = environ; *e != nullptr; ++e) {
            std::string entry(*e);
            size_t eq = entry.find('=');
            if (eq == std::string::npos) continue;
            std::string k = entry.substr(0, eq);
            std::string k_upper = k;
            for (char &ch : k_upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            if (!is_dangerous_env(k_upper)) {
                clean_env_storage.push_back(std::move(entry));
            }
        }
    }
    for (auto &s : clean_env_storage) {
        clean_envp.push_back(const_cast<char *>(s.c_str()));
    }
    clean_envp.push_back(nullptr);

    pid_t pid = -1;
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
#if defined(__APPLE__) && defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
    int status = posix_spawnp(&pid, program, &actions, &attr, argv.data(), clean_envp.data());
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);

    close(pipe_fds[1]);

    if (status != 0) {
        close(pipe_fds[0]);
        return nullptr;
    }

    ProcessHandle *proc = new ProcessHandle();
    proc->pid = pid;
    proc->pipe_fd = pipe_fds[0];
    proc->exited = false;
    proc->exit_code = 0;
    return proc;
#endif
}

int platform_process_read_line(void *handle, char *out_buf, int max_len) {
    if (!handle || !out_buf || max_len <= 0) return -1;
    ProcessHandle *proc = static_cast<ProcessHandle *>(handle);
    static constexpr size_t MAX_LINE_BUFFER = 4 * 1024 * 1024;

    while (true) {
        size_t newline_pos = proc->line_buffer.find('\n');
        if (newline_pos != std::string::npos) {
            std::string line = proc->line_buffer.substr(0, newline_pos);
            proc->line_buffer.erase(0, newline_pos + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            size_t copy_len = std::min(static_cast<size_t>(max_len - 1), line.size());
            memcpy(out_buf, line.data(), copy_len);
            out_buf[copy_len] = '\0';
            return static_cast<int>(copy_len);
        }

        if (proc->line_buffer.size() >= MAX_LINE_BUFFER) {
            std::string line = proc->line_buffer.substr(0, static_cast<size_t>(max_len - 1));
            proc->line_buffer.clear();
            memcpy(out_buf, line.data(), line.size());
            out_buf[line.size()] = '\0';
            return static_cast<int>(line.size());
        }

#if defined(_WIN32)
        char temp_buf[4096];
        DWORD bytes_read = 0;
        BOOL success = ReadFile(proc->hRead, temp_buf, sizeof(temp_buf), &bytes_read, NULL);
        if (success && bytes_read > 0) {
            proc->line_buffer.append(temp_buf, bytes_read);
        } else {
            if (!proc->line_buffer.empty()) {
                std::string line = proc->line_buffer;
                proc->line_buffer.clear();
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                size_t copy_len = std::min(static_cast<size_t>(max_len - 1), line.size());
                memcpy(out_buf, line.data(), copy_len);
                out_buf[copy_len] = '\0';
                return static_cast<int>(copy_len);
            }
            return -1;
        }
#else
        char temp_buf[4096];
        ssize_t bytes_read = read(proc->pipe_fd, temp_buf, sizeof(temp_buf));
        if (bytes_read < 0 && errno == EINTR) {
            continue;
        }
        if (bytes_read > 0) {
            proc->line_buffer.append(temp_buf, static_cast<size_t>(bytes_read));
        } else {
            if (!proc->line_buffer.empty()) {
                std::string line = proc->line_buffer;
                proc->line_buffer.clear();
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                size_t copy_len = std::min(static_cast<size_t>(max_len - 1), line.size());
                memcpy(out_buf, line.data(), copy_len);
                out_buf[copy_len] = '\0';
                return static_cast<int>(copy_len);
            }
            return -1;
        }
#endif
    }
}

int platform_process_wait(void *handle) {
    if (!handle) return -1;
    ProcessHandle *proc = static_cast<ProcessHandle *>(handle);
#if defined(_WIN32)
    if (proc->hProcess) {
        WaitForSingleObject(proc->hProcess, INFINITE);
        DWORD exit_code = 0;
        GetExitCodeProcess(proc->hProcess, &exit_code);
        return static_cast<int>(exit_code);
    }
    return -1;
#else
    if (proc->exited) return proc->exit_code;
    if (proc->pid > 0) {
        int status = 0;
        pid_t wres;
        do {
            wres = waitpid(proc->pid, &status, 0);
        } while (wres < 0 && errno == EINTR);
        if (wres < 0) return -1;
        proc->exited = true;
        if (WIFEXITED(status)) {
            proc->exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            proc->exit_code = 128 + WTERMSIG(status);
        } else {
            proc->exit_code = 1;
        }
        return proc->exit_code;
    }
    return -1;
#endif
}

void platform_process_close(void *handle) {
    if (!handle) return;
    ProcessHandle *proc = static_cast<ProcessHandle *>(handle);
#if defined(_WIN32)
    if (proc->hRead) {
        CloseHandle(proc->hRead);
        proc->hRead = NULL;
    }
    if (proc->hThread) {
        CloseHandle(proc->hThread);
        proc->hThread = NULL;
    }
    if (proc->hProcess) {
        CloseHandle(proc->hProcess);
        proc->hProcess = NULL;
    }
#else
    if (proc->pipe_fd >= 0) {
        close(proc->pipe_fd);
        proc->pipe_fd = -1;
    }
    if (!proc->exited && proc->pid > 0) {
        int status = 0;
        waitpid(proc->pid, &status, WNOHANG);
    }
#endif
    delete proc;
}

static bool is_dangerous_child_env_key(const std::string &key_upper) {
    if (key_upper.rfind("LD_", 0) == 0 || key_upper.rfind("DYLD_", 0) == 0) return true;
    static const char *blocked[] = {
        "NODE_OPTIONS", "PYTHONPATH", "PYTHONSTARTUP", "BASH_ENV", "ENV", "IFS",
        "GIT_SSH_COMMAND", "GIT_EXEC_PATH", "PERL5OPT", "PERL5LIB", "RUBYOPT",
        "JAVA_TOOL_OPTIONS", "_JAVA_OPTIONS", "GCONV_PATH", "HOSTALIASES",
        "MALLOC_TRACE", "SSLKEYLOGFILE", "SHELLOPTS", "PS4", "ZDOTDIR",
        "DOTNET_STARTUP_HOOKS", "CDPATH", "LUA_PATH", "LUA_CPATH", "LUA_INIT"
    };
    for (const char *b : blocked) {
        if (key_upper == b) return true;
    }
    return false;
}

long platform_sidecar_spawn(const char *program, const char **args, int arg_count, const char **env_kv, int env_count, const char *cwd, const char *log_file) {
    if (!program || program[0] == '\0') return -1;
    if (arg_count < 0 || arg_count > 1024 || env_count < 0 || env_count > 1024) return -1;

#if defined(_WIN32)
    auto to_wide = [](const std::string &s) -> std::wstring {
        if (s.empty()) return L"";
        int wlen = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), NULL, 0);
        if (wlen <= 0) return L"";
        std::wstring w(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], wlen);
        return w;
    };

    std::string prog_str(program);
    std::string prog_lower = prog_str;
    while (!prog_lower.empty() && (prog_lower.back() == '.' || prog_lower.back() == ' ')) prog_lower.pop_back();
    for (char &c : prog_lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (prog_lower.find(':', 2) != std::string::npos) return -1;
    auto ends_with = [&](const char *ext) {
        size_t elen = std::strlen(ext);
        return prog_lower.size() >= elen && prog_lower.compare(prog_lower.size() - elen, elen, ext) == 0;
    };
    if (ends_with(".bat") || ends_with(".cmd") || ends_with(".vbs") || ends_with(".vbe") ||
        ends_with(".js") || ends_with(".jse") || ends_with(".ps1") || ends_with(".hta") || ends_with(".wsf")) {
        return -1;
    }

    std::wstring prog_w = to_wide(prog_str);
    std::wstring cmd_line_w;
    auto append_quoted_w = [&](const std::string &arg) {
        if (!cmd_line_w.empty()) cmd_line_w += L" ";
        std::wstring warg = to_wide(arg);
        bool needs_quotes = warg.empty() || (warg.find_first_of(L" \t\n\v\"") != std::wstring::npos);
        if (!needs_quotes) {
            cmd_line_w += warg;
        } else {
            cmd_line_w += L"\"";
            size_t backslashes = 0;
            for (wchar_t c : warg) {
                if (c == L'\\') {
                    backslashes++;
                } else if (c == L'\"') {
                    cmd_line_w.append(backslashes * 2 + 1, L'\\');
                    cmd_line_w += L'\"';
                    backslashes = 0;
                } else {
                    cmd_line_w.append(backslashes, L'\\');
                    backslashes = 0;
                    cmd_line_w += c;
                }
            }
            cmd_line_w.append(backslashes * 2, L'\\');
            cmd_line_w += L"\"";
        }
    };
    append_quoted_w(prog_str);
    for (int i = 0; i < arg_count; ++i) {
        if (args && args[i]) append_quoted_w(args[i]);
    }

    std::map<std::wstring, std::wstring> env_map;
    LPWCH raw_env = GetEnvironmentStringsW();
    if (raw_env) {
        const wchar_t *p = raw_env;
        while (*p != L'\0') {
            std::wstring entry(p);
            p += entry.size() + 1;
            size_t eq = entry.find(L'=');
            if (eq == std::wstring::npos || eq == 0) continue;
            std::wstring wk = entry.substr(0, eq);
            std::string ku;
            for (wchar_t wc : wk) {
                if (wc < 128) ku.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(wc))));
            }
            if (!is_dangerous_child_env_key(ku)) {
                env_map[wk] = entry.substr(eq + 1);
            }
        }
        FreeEnvironmentStringsW(raw_env);
    }
    for (int i = 0; i < env_count; ++i) {
        if (!env_kv || !env_kv[i]) continue;
        std::string kv(env_kv[i]);
        size_t eq = kv.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::string k = kv.substr(0, eq);
        std::string ku = k;
        for (char &ch : ku) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (ku == "PATH" || is_dangerous_child_env_key(ku)) continue;
        env_map[to_wide(k)] = to_wide(kv.substr(eq + 1));
    }
    std::vector<wchar_t> env_block;
    for (const auto &kv : env_map) {
        env_block.insert(env_block.end(), kv.first.begin(), kv.first.end());
        env_block.push_back(L'=');
        env_block.insert(env_block.end(), kv.second.begin(), kv.second.end());
        env_block.push_back(L'\0');
    }
    env_block.push_back(L'\0');

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE hLog = INVALID_HANDLE_VALUE;
    if (log_file && log_file[0] != '\0' && std::strcmp(log_file, "NUL") != 0) {
        std::wstring log_w = to_wide(log_file);
        DWORD attrs = GetFileAttributesW(log_w.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
            hLog = CreateFileW(log_w.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        }
    }
    if (hLog == INVALID_HANDLE_VALUE) {
        hLog = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    HANDLE hNullIn = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hNullIn;
    si.hStdOutput = hLog;
    si.hStdError = hLog;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    std::vector<wchar_t> cmd_buf(cmd_line_w.begin(), cmd_line_w.end());
    cmd_buf.push_back(L'\0');

    std::wstring cwd_w = (cwd && cwd[0] != '\0') ? to_wide(cwd) : L"";
    BOOL ok = CreateProcessW(
        prog_w.c_str(),
        cmd_buf.data(),
        NULL,
        NULL,
        TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        env_block.data(),
        cwd_w.empty() ? NULL : cwd_w.c_str(),
        &si,
        &pi
    );

    if (hLog != INVALID_HANDLE_VALUE) CloseHandle(hLog);
    if (hNullIn != INVALID_HANDLE_VALUE) CloseHandle(hNullIn);
    if (!ok) return -1;

    long pid = static_cast<long>(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    platform_register_child_pid(pid);
    return pid;
#else
    std::vector<char *> argv;
    argv.push_back(const_cast<char *>(program));
    for (int i = 0; i < arg_count; ++i) {
        if (args && args[i]) {
            argv.push_back(const_cast<char *>(args[i]));
        }
    }
    argv.push_back(nullptr);

    std::map<std::string, std::string> env_map;
    if (environ) {
        for (char **e = environ; *e != nullptr; ++e) {
            std::string entry(*e);
            size_t eq = entry.find('=');
            if (eq == std::string::npos || eq == 0) continue;
            std::string k = entry.substr(0, eq);
            std::string ku = k;
            for (char &ch : ku) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            if (!is_dangerous_child_env_key(ku)) {
                env_map[k] = entry.substr(eq + 1);
            }
        }
    }
    for (int i = 0; i < env_count; ++i) {
        if (!env_kv || !env_kv[i]) continue;
        std::string kv(env_kv[i]);
        size_t eq = kv.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::string k = kv.substr(0, eq);
        bool valid_key = (std::isalpha(static_cast<unsigned char>(k[0])) || k[0] == '_');
        for (size_t j = 1; valid_key && j < k.size(); ++j) {
            if (!std::isalnum(static_cast<unsigned char>(k[j])) && k[j] != '_') valid_key = false;
        }
        if (!valid_key) continue;
        std::string ku = k;
        for (char &ch : ku) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (ku == "PATH" || is_dangerous_child_env_key(ku)) continue;
        env_map[k] = kv.substr(eq + 1);
    }

    std::vector<std::string> env_storage;
    std::vector<char *> envp;
    env_storage.reserve(env_map.size());
    for (const auto &kv : env_map) {
        env_storage.push_back(kv.first + "=" + kv.second);
    }
    for (auto &s : env_storage) {
        envp.push_back(const_cast<char *>(s.c_str()));
    }
    envp.push_back(nullptr);

    int log_fd = -1;
    if (log_file && log_file[0] != '\0' && std::strcmp(log_file, "/dev/null") != 0) {
        int flags = O_WRONLY | O_CREAT | O_APPEND;
#if defined(O_NOFOLLOW)
        flags |= O_NOFOLLOW;
#endif
#if defined(O_CLOEXEC)
        flags |= O_CLOEXEC;
#endif
        log_fd = ::open(log_file, flags, 0600);
        if (log_fd >= 0) {
            struct stat st;
            if (fstat(log_fd, &st) != 0 || !S_ISREG(st.st_mode)) {
                ::close(log_fd);
                log_fd = -1;
            }
        }
    }
    if (log_fd < 0) {
        int flags = O_WRONLY;
#if defined(O_CLOEXEC)
        flags |= O_CLOEXEC;
#endif
        log_fd = ::open("/dev/null", flags);
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (log_fd >= 0) {
        posix_spawn_file_actions_adddup2(&actions, log_fd, STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, log_fd, STDERR_FILENO);
        if (log_fd != STDOUT_FILENO && log_fd != STDERR_FILENO) {
            posix_spawn_file_actions_addclose(&actions, log_fd);
        }
    }
#if defined(__APPLE__) || (defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 29)))
    if (cwd && cwd[0] != '\0') {
        posix_spawn_file_actions_addchdir_np(&actions, cwd);
    }
#endif

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    short spawn_flags = POSIX_SPAWN_SETPGROUP;
#if defined(__APPLE__) && defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    spawn_flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    posix_spawnattr_setflags(&attr, spawn_flags);
    posix_spawnattr_setpgroup(&attr, 0);

    pid_t pid = -1;
    int status = posix_spawnp(&pid, program, &actions, &attr, argv.data(), envp.data());
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    if (log_fd >= 0) {
        ::close(log_fd);
    }
    if (status != 0 || pid <= 0) {
        return -1;
    }
    platform_register_child_pid(static_cast<long>(pid));
    return static_cast<long>(pid);
#endif
}

int platform_sidecar_is_running(long pid) {
    if (pid <= 0 || pid > std::numeric_limits<int32_t>::max()) return 0;
#if defined(_WIN32)
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!hProc) return 0;
    DWORD exit_code = 0;
    BOOL ok = GetExitCodeProcess(hProc, &exit_code);
    CloseHandle(hProc);
    return (ok && exit_code == STILL_ACTIVE) ? 1 : 0;
#else
    pid_t p = static_cast<pid_t>(pid);
    int st = 0;
    pid_t w = waitpid(p, &st, WNOHANG);
    if (w == p) return 0;
    if (kill(p, 0) == 0) return 1;
    return (errno == EPERM) ? 1 : 0;
#endif
}

int platform_sidecar_kill(long pid, int sig) {
    if (pid <= 0 || pid > std::numeric_limits<int32_t>::max()) return 0;
    platform_unregister_child_pid(pid);
#if defined(_WIN32)
    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (!hProc) return 0;
    BOOL ok = TerminateProcess(hProc, 1);
    CloseHandle(hProc);
    return ok ? 1 : 0;
#else
    pid_t p = static_cast<pid_t>(pid);
    int s = (sig > 0) ? sig : SIGTERM;
    kill(p, s);
    int st = 0;
    for (int i = 0; i < 15; ++i) {
        pid_t w = waitpid(p, &st, WNOHANG);
        if (w == p || (w < 0 && errno == ECHILD)) {
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (sig <= 0) {
        pid_t w = waitpid(p, &st, WNOHANG);
        if (w == 0) {
            kill(p, SIGKILL);
            while (waitpid(p, &st, 0) < 0 && errno == EINTR) {}
        }
    }
    return 1;
#endif
}

void platform_menu_set_callback(menu_callback_fn on_menu) {
    g_menu_cb = on_menu;
}

void platform_window_set_close_callback(window_close_callback_fn on_close) {
    g_window_close_cb = on_close;
}

#if defined(__APPLE__)
@interface TauriMenuHandler : NSObject
+ (TauriMenuHandler *)sharedInstance;
- (void)menuItemClicked:(NSMenuItem *)sender;
@end

@implementation TauriMenuHandler
+ (TauriMenuHandler *)sharedInstance {
    static TauriMenuHandler *instance = nil;
    if (!instance) {
        instance = [[TauriMenuHandler alloc] init];
    }
    return instance;
}

- (void)menuItemClicked:(NSMenuItem *)sender {
    NSString *itemId = sender.representedObject;
    if (itemId && g_menu_cb) {
        g_menu_cb([itemId UTF8String]);
    }
}
@end

static void parse_accelerator(const std::string &accel, NSString *__strong *out_key, NSEventModifierFlags *out_flags) {
    *out_key = @"";
    *out_flags = 0;
    if (accel.empty()) return;

    std::string cleaned = accel;
    size_t coc_pos = 0;
    while ((coc_pos = cleaned.find("CmdOrCtrl")) != std::string::npos) {
        *out_flags |= NSEventModifierFlagCommand;
        cleaned.replace(coc_pos, 9, "");
    }
    if (cleaned.find("Shift") != std::string::npos) {
        *out_flags |= NSEventModifierFlagShift;
    }
    if (cleaned.find("Command") != std::string::npos || cleaned.find("Cmd") != std::string::npos) {
        *out_flags |= NSEventModifierFlagCommand;
    }
    if (cleaned.find("Alt") != std::string::npos || cleaned.find("Option") != std::string::npos) {
        *out_flags |= NSEventModifierFlagOption;
    }
    if (cleaned.find("Control") != std::string::npos || cleaned.find("Ctrl") != std::string::npos) {
        *out_flags |= NSEventModifierFlagControl;
    }

    size_t last_plus = accel.rfind('+');
    std::string key = (last_plus != std::string::npos) ? accel.substr(last_plus + 1) : accel;
    if (!key.empty()) {
        std::string lower_key = key;
        for (char &c : lower_key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        *out_key = webview_nsstring_utf8(lower_key);
    }
}

static void build_nsmenu_items(NSMenu *menu, const std::vector<JsonVal> &items, int depth = 0) {
    if (depth > 16) return;
    for (const auto &item : items) {
        std::string type = item["type"].str();
        std::string role = item["role"].str();
        std::string label = item["label"].str();
        if (label.empty()) label = item["text"].str();
        std::string id = item["id"].str();
        std::string accel = item["accelerator"].str();
        bool enabled = (item["enabled"].type == JsonVal::Bool) ? item["enabled"].boolean() : true;

        if (type == "separator" || role == "separator") {
            [menu addItem:[NSMenuItem separatorItem]];
            continue;
        }

        if (type == "submenu" || !item["items"].arr.empty()) {
            NSString *nsTitle = webview_nsstring_utf8(label);
            NSMenuItem *subParent = [[NSMenuItem alloc] initWithTitle:nsTitle action:nil keyEquivalent:@""];
            NSMenu *sub = [[NSMenu alloc] initWithTitle:nsTitle];
            build_nsmenu_items(sub, item["items"].arr, depth + 1);
            [subParent setSubmenu:sub];
            [menu addItem:subParent];
            continue;
        }

        SEL action = nil;
        NSString *keyEq = @"";
        NSEventModifierFlags modFlags = 0;

        if (!role.empty()) {
            if (role == "about") {
                action = @selector(orderFrontStandardAboutPanel:);
            } else if (role == "hide") {
                action = @selector(hide:);
                keyEq = @"h";
                modFlags = NSEventModifierFlagCommand;
            } else if (role == "hide_others") {
                action = @selector(hideOtherApplications:);
                keyEq = @"h";
                modFlags = NSEventModifierFlagOption | NSEventModifierFlagCommand;
            } else if (role == "show_all") {
                action = @selector(unhideAllApplications:);
            } else if (role == "quit") {
                action = @selector(terminate:);
                keyEq = @"q";
                modFlags = NSEventModifierFlagCommand;
            } else if (role == "undo") {
                action = @selector(undo:);
                keyEq = @"z";
                modFlags = NSEventModifierFlagCommand;
            } else if (role == "redo") {
                action = @selector(redo:);
                keyEq = @"z";
                modFlags = NSEventModifierFlagShift | NSEventModifierFlagCommand;
            } else if (role == "cut") {
                action = @selector(cut:);
                keyEq = @"x";
                modFlags = NSEventModifierFlagCommand;
            } else if (role == "copy") {
                action = @selector(copy:);
                keyEq = @"c";
                modFlags = NSEventModifierFlagCommand;
            } else if (role == "paste") {
                action = @selector(paste:);
                keyEq = @"v";
                modFlags = NSEventModifierFlagCommand;
            } else if (role == "select_all") {
                action = @selector(selectAll:);
                keyEq = @"a";
                modFlags = NSEventModifierFlagCommand;
            } else if (role == "minimize") {
                action = @selector(performMiniaturize:);
                keyEq = @"m";
                modFlags = NSEventModifierFlagCommand;
            } else if (role == "zoom") {
                action = @selector(performZoom:);
            } else if (role == "fullscreen") {
                action = @selector(toggleFullScreen:);
                keyEq = @"f";
                modFlags = NSEventModifierFlagControl | NSEventModifierFlagCommand;
            } else if (role == "close" || role == "close_window") {
                action = @selector(performClose:);
                keyEq = @"w";
                modFlags = NSEventModifierFlagCommand;
            }
        }

        if (action != nil) {
            NSString *nsTitle = webview_nsstring_utf8(label);
            NSMenuItem *mi = [[NSMenuItem alloc] initWithTitle:nsTitle action:action keyEquivalent:keyEq];
            if (modFlags != 0) {
                [mi setKeyEquivalentModifierMask:modFlags];
            }
            if (role == "quit" || role == "hide" || role == "hide_others" || role == "show_all") {
                [mi setTarget:NSApp];
            }
            [menu addItem:mi];
        } else {
            parse_accelerator(accel, &keyEq, &modFlags);
            NSString *nsTitle = webview_nsstring_utf8(label);
            NSMenuItem *mi = [[NSMenuItem alloc] initWithTitle:nsTitle action:@selector(menuItemClicked:) keyEquivalent:keyEq];
            if (modFlags != 0) {
                [mi setKeyEquivalentModifierMask:modFlags];
            }
            [mi setTarget:[TauriMenuHandler sharedInstance]];
            mi.representedObject = webview_nsstring_utf8(id);
            if (!enabled) {
                [mi setEnabled:NO];
            }
            if (type == "check" && item["checked"].boolean()) {
                [mi setState:NSControlStateValueOn];
            }
            [menu addItem:mi];
        }
    }
}
#elif defined(_WIN32)
static UINT g_next_menu_id = 20000;
static std::mutex g_win32_menu_mutex;
static std::map<UINT, std::string> g_win32_menu_id_to_action;
static std::map<HWND, WNDPROC> g_original_wndprocs;
static std::map<HWND, HMENU> g_attached_menus;
static std::string g_last_menu_json;

static std::wstring utf8_to_wide(const std::string &str) {
    if (str.empty()) return L"";
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

static LRESULT CALLBACK Win32MenuSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND) {
        UINT cmd = LOWORD(wp);
        std::string action;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(g_win32_menu_mutex);
            auto it = g_win32_menu_id_to_action.find(cmd);
            if (it != g_win32_menu_id_to_action.end()) {
                action = it->second;
                found = true;
            }
        }
        if (found) {
            if (action == "role:quit") {
                PostMessage(hwnd, WM_CLOSE, 0, 0);
            } else if (action == "role:close" || action == "role:close_window") {
                SendMessage(hwnd, WM_CLOSE, 0, 0);
            } else if (action == "role:minimize") {
                ShowWindow(hwnd, SW_MINIMIZE);
            } else if (action == "role:zoom") {
                WINDOWPLACEMENT wp_info;
                wp_info.length = sizeof(WINDOWPLACEMENT);
                GetWindowPlacement(hwnd, &wp_info);
                if (wp_info.showCmd == SW_SHOWMAXIMIZED) {
                    ShowWindow(hwnd, SW_RESTORE);
                } else {
                    ShowWindow(hwnd, SW_MAXIMIZE);
                }
            } else if (action == "role:fullscreen") {
                static bool is_full = false;
                static RECT prev_rect;
                if (!is_full) {
                    GetWindowRect(hwnd, &prev_rect);
                    SetWindowLong(hwnd, GWL_STYLE, GetWindowLong(hwnd, GWL_STYLE) & ~WS_OVERLAPPEDWINDOW);
                    ShowWindow(hwnd, SW_MAXIMIZE);
                    is_full = true;
                } else {
                    SetWindowLong(hwnd, GWL_STYLE, GetWindowLong(hwnd, GWL_STYLE) | WS_OVERLAPPEDWINDOW);
                    SetWindowPos(hwnd, NULL, prev_rect.left, prev_rect.top, prev_rect.right - prev_rect.left, prev_rect.bottom - prev_rect.top, SWP_NOZORDER | SWP_FRAMECHANGED);
                    ShowWindow(hwnd, SW_RESTORE);
                    is_full = false;
                }
            } else if (action.rfind("role:", 0) == 0) {
            } else {
                if (g_menu_cb) {
                    g_menu_cb(action.c_str());
                }
            }
            return 0;
        }
    }
    WNDPROC orig = NULL;
    {
        std::lock_guard<std::mutex> lock(g_win32_menu_mutex);
        auto proc_it = g_original_wndprocs.find(hwnd);
        if (proc_it != g_original_wndprocs.end()) {
            orig = proc_it->second;
        }
    }
    if (orig) {
        return CallWindowProcW(orig, hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void build_win32_menu_items(HMENU parent, const std::vector<JsonVal> &items, int depth = 0) {
    if (depth > 16) return;
    for (const auto &item : items) {
        std::string type = item["type"].str();
        std::string role = item["role"].str();
        std::string label = item["label"].str();
        if (label.empty()) label = item["text"].str();
        std::string id = item["id"].str();
        std::string accel = item["accelerator"].str();
        bool enabled = (item["enabled"].type == JsonVal::Bool) ? item["enabled"].boolean() : true;
        bool checked = (item["checked"].type == JsonVal::Bool) ? item["checked"].boolean() : false;

        if (type == "separator" || role == "separator") {
            AppendMenuW(parent, MF_SEPARATOR, 0, NULL);
            continue;
        }

        std::string display_title = label;
        if (!accel.empty()) {
            display_title += "\t" + accel;
        }
        std::wstring wtitle = utf8_to_wide(display_title);

        if (type == "submenu" || !item["items"].arr.empty()) {
            HMENU hSub = CreatePopupMenu();
            build_win32_menu_items(hSub, item["items"].arr, depth + 1);
            UINT flags = MF_POPUP;
            if (!enabled) flags |= MF_GRAYED;
            AppendMenuW(parent, flags, (UINT_PTR)hSub, wtitle.c_str());
            continue;
        }

        UINT cmd_id = g_next_menu_id++;
        UINT flags = MF_STRING;
        if (!enabled) flags |= MF_GRAYED;
        if (type == "check" && checked) flags |= MF_CHECKED;

        if (!role.empty()) {
            g_win32_menu_id_to_action[cmd_id] = "role:" + role;
        } else {
            g_win32_menu_id_to_action[cmd_id] = id;
        }

        AppendMenuW(parent, flags, cmd_id, wtitle.c_str());
    }
}

static void apply_menu_to_win32_window(HWND hwnd, const std::vector<JsonVal> &items) {
    if (!hwnd) return;
    if (g_original_wndprocs.find(hwnd) == g_original_wndprocs.end()) {
        WNDPROC old = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)Win32MenuSubclassProc);
        g_original_wndprocs[hwnd] = old;
    }
    HMENU hMenuBar = CreateMenu();
    build_win32_menu_items(hMenuBar, items);
    auto it = g_attached_menus.find(hwnd);
    if (it != g_attached_menus.end() && it->second) {
        DestroyMenu(it->second);
    }
    g_attached_menus[hwnd] = hMenuBar;
    SetMenu(hwnd, hMenuBar);
    DrawMenuBar(hwnd);
}

void win32_attach_current_menu(HWND hwnd) {
    if (g_last_menu_json.empty() || !hwnd) return;
    JsonVal root;
    if (!parse_json_strict(g_last_menu_json, root, 32)) return;
    const std::vector<JsonVal> *items_to_build = nullptr;
    if (root.type == JsonVal::Array) {
        items_to_build = &root.arr;
    } else if (root.type == JsonVal::Object && root.obj.count("items") && root.obj["items"].type == JsonVal::Array) {
        items_to_build = &root.obj["items"].arr;
    }
    if (items_to_build) {
        apply_menu_to_win32_window(hwnd, *items_to_build);
    }
}
#else
static std::string g_last_menu_json_linux;

static void on_gtk_menu_item_activate(GtkMenuItem *menuitem, gpointer user_data) {
    char *action = static_cast<char *>(user_data);
    if (!action) return;
    std::string s_action(action);

    if (s_action.rfind("role:", 0) == 0) {
        std::string role = s_action.substr(5);
        GtkWidget *toplevel = gtk_widget_get_toplevel(GTK_WIDGET(menuitem));
        if (toplevel && GTK_IS_WINDOW(toplevel)) {
            if (role == "quit" || role == "close" || role == "close_window") {
                gtk_window_close(GTK_WINDOW(toplevel));
            } else if (role == "minimize") {
                gtk_window_iconify(GTK_WINDOW(toplevel));
            } else if (role == "zoom") {
                if (gtk_window_is_maximized(GTK_WINDOW(toplevel))) {
                    gtk_window_unmaximize(GTK_WINDOW(toplevel));
                } else {
                    gtk_window_maximize(GTK_WINDOW(toplevel));
                }
            } else if (role == "fullscreen") {
                static bool is_full = false;
                if (!is_full) {
                    gtk_window_fullscreen(GTK_WINDOW(toplevel));
                    is_full = true;
                } else {
                    gtk_window_unfullscreen(GTK_WINDOW(toplevel));
                    is_full = false;
                }
            }
        }
    } else {
        if (g_menu_cb) {
            g_menu_cb(s_action.c_str());
        }
    }
}

static void build_gtk_menu_items(GtkWidget *parent_shell, const std::vector<JsonVal> &items, GtkAccelGroup *accel_group, int depth = 0) {
    if (depth > 16) return;
    for (const auto &item : items) {
        std::string type = item["type"].str();
        std::string role = item["role"].str();
        std::string label = item["label"].str();
        if (label.empty()) label = item["text"].str();
        std::string id = item["id"].str();
        std::string accel = item["accelerator"].str();
        bool enabled = (item["enabled"].type == JsonVal::Bool) ? item["enabled"].boolean() : true;
        bool checked = (item["checked"].type == JsonVal::Bool) ? item["checked"].boolean() : false;

        if (type == "separator" || role == "separator") {
            GtkWidget *sep = gtk_separator_menu_item_new();
            gtk_menu_shell_append(GTK_MENU_SHELL(parent_shell), sep);
            continue;
        }

        if (type == "submenu" || !item["items"].arr.empty()) {
            GtkWidget *sub_item = gtk_menu_item_new_with_label(label.c_str());
            GtkWidget *sub_menu = gtk_menu_new();
            gtk_menu_item_set_submenu(GTK_MENU_ITEM(sub_item), sub_menu);
            build_gtk_menu_items(sub_menu, item["items"].arr, accel_group, depth + 1);
            gtk_menu_shell_append(GTK_MENU_SHELL(parent_shell), sub_item);
            continue;
        }

        GtkWidget *item_widget = nullptr;
        if (type == "check") {
            item_widget = gtk_check_menu_item_new_with_label(label.c_str());
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item_widget), checked ? TRUE : FALSE);
        } else {
            item_widget = gtk_menu_item_new_with_label(label.c_str());
        }

        if (!enabled) {
            gtk_widget_set_sensitive(item_widget, FALSE);
        }

        std::string action = (!role.empty()) ? ("role:" + role) : id;
        g_signal_connect_data(item_widget, "activate", G_CALLBACK(on_gtk_menu_item_activate), strdup(action.c_str()), +[](gpointer d, GClosure *) { free(d); }, (GConnectFlags)0);

        if (!accel.empty() && accel_group) {
            guint key = 0;
            GdkModifierType mods = (GdkModifierType)0;
            std::string gtk_accel = accel;
            size_t p = 0;
            while ((p = gtk_accel.find("CmdOrCtrl")) != std::string::npos) {
                gtk_accel.replace(p, 9, "<Primary>");
            }
            while ((p = gtk_accel.find("Shift")) != std::string::npos) {
                gtk_accel.replace(p, 5, "<Shift>");
            }
            while ((p = gtk_accel.find("Alt")) != std::string::npos) {
                gtk_accel.replace(p, 3, "<Alt>");
            }
            while ((p = gtk_accel.find("Control")) != std::string::npos) {
                gtk_accel.replace(p, 7, "<Control>");
            }
            while ((p = gtk_accel.find("+")) != std::string::npos) {
                gtk_accel.erase(p, 1);
            }
            gtk_accelerator_parse(gtk_accel.c_str(), &key, &mods);
            if (key != 0) {
                gtk_widget_add_accelerator(item_widget, "activate", accel_group, key, mods, GTK_ACCEL_VISIBLE);
            }
        }

        gtk_menu_shell_append(GTK_MENU_SHELL(parent_shell), item_widget);
    }
}

static void apply_menu_to_gtk_window(GtkWindow *gtkwin, const std::vector<JsonVal> &items) {
    if (!gtkwin) return;
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(gtkwin));
    GtkWidget *vbox = nullptr;
    GtkWidget *webview_widget = nullptr;

    if (GTK_IS_BOX(child)) {
        vbox = child;
        GList *children = gtk_container_get_children(GTK_CONTAINER(vbox));
        if (children && GTK_IS_MENU_BAR(children->data)) {
            gtk_widget_destroy(GTK_WIDGET(children->data));
        }
        if (children) g_list_free(children);
    } else {
        webview_widget = child;
        g_object_ref(webview_widget);
        gtk_container_remove(GTK_CONTAINER(gtkwin), webview_widget);

        vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_container_add(GTK_CONTAINER(gtkwin), vbox);
        gtk_box_pack_end(GTK_BOX(vbox), webview_widget, TRUE, TRUE, 0);
        g_object_unref(webview_widget);
    }

    GtkWidget *menu_bar = gtk_menu_bar_new();
    GtkAccelGroup *accel_group = gtk_accel_group_new();
    gtk_window_add_accel_group(gtkwin, accel_group);

    build_gtk_menu_items(menu_bar, items, accel_group);
    gtk_box_pack_start(GTK_BOX(vbox), menu_bar, FALSE, FALSE, 0);
    gtk_widget_show_all(menu_bar);
    gtk_widget_show_all(vbox);
}

void gtk_attach_current_menu(GtkWindow *gtkwin) {
    if (g_last_menu_json_linux.empty() || !gtkwin) return;
    JsonVal root;
    if (!parse_json_strict(g_last_menu_json_linux, root, 32)) return;
    const std::vector<JsonVal> *items_to_build = nullptr;
    if (root.type == JsonVal::Array) {
        items_to_build = &root.arr;
    } else if (root.type == JsonVal::Object && root.obj.count("items") && root.obj["items"].type == JsonVal::Array) {
        items_to_build = &root.obj["items"].arr;
    }
    if (items_to_build) {
        apply_menu_to_gtk_window(gtkwin, *items_to_build);
    }
}
#endif

void platform_set_menu(const char *json_menu) {
    if (!json_menu) return;
    std::string s_json(json_menu);
    JsonVal root;
    if (!parse_json_strict(s_json, root, 32)) return;
    const std::vector<JsonVal> *items_to_build = nullptr;
    if (root.type == JsonVal::Array) {
        items_to_build = &root.arr;
    } else if (root.type == JsonVal::Object && root.obj.count("items") && root.obj["items"].type == JsonVal::Array) {
        items_to_build = &root.obj["items"].arr;
    }
    if (!items_to_build) return;

#if defined(__APPLE__)
    run_on_main_sync(^{
        NSString *appName = nil;
        if (!g_app_name.empty()) {
            appName = [NSString stringWithUTF8String:g_app_name.c_str()];
        }
        if (!appName || [appName length] == 0) {
            appName = [[[NSBundle mainBundle] infoDictionary] objectForKey:@"CFBundleName"];
        }
        if (!appName || [appName length] == 0) {
            appName = [[NSProcessInfo processInfo] processName];
        }
        if (!appName || [appName length] == 0) {
            appName = @"Tauri App";
        }

        [[NSProcessInfo processInfo] setProcessName:appName];

        NSMenu *mainMenu = [[NSMenu alloc] init];

        bool hasCustomAppMenu = false;
        if (!items_to_build->empty()) {
            const JsonVal &first = (*items_to_build)[0];
            std::string first_title = first["label"].str();
            if (first_title.empty()) first_title = first["text"].str();
            if (!first_title.empty() && [appName isEqualToString:[NSString stringWithUTF8String:first_title.c_str()]]) {
                hasCustomAppMenu = true;
            }
        }

        if (!hasCustomAppMenu) {
            // 1. Build default App Menu when caller didn't supply one
            NSMenuItem *appMenuItem = [[NSMenuItem alloc] init];
            [appMenuItem setTitle:appName];
            [mainMenu addItem:appMenuItem];
            NSMenu *appMenu = [[NSMenu alloc] initWithTitle:appName];
            [appMenuItem setSubmenu:appMenu];

            NSString *aboutTitle = [NSString stringWithFormat:@"About %@", appName];
            [appMenu addItemWithTitle:aboutTitle action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
            [appMenu addItem:[NSMenuItem separatorItem]];

            NSMenuItem *servicesItem = [[NSMenuItem alloc] init];
            [servicesItem setTitle:@"Services"];
            [appMenu addItem:servicesItem];
            NSMenu *servicesMenu = [[NSMenu alloc] initWithTitle:@"Services"];
            [servicesItem setSubmenu:servicesMenu];
            [NSApp setServicesMenu:servicesMenu];
            [appMenu addItem:[NSMenuItem separatorItem]];

            NSString *hideTitle = [NSString stringWithFormat:@"Hide %@", appName];
            [appMenu addItemWithTitle:hideTitle action:@selector(hide:) keyEquivalent:@"h"];
            NSMenuItem *hideOthers = [[NSMenuItem alloc] initWithTitle:@"Hide Others" action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
            [hideOthers setKeyEquivalentModifierMask:(NSEventModifierFlagOption | NSEventModifierFlagCommand)];
            [appMenu addItem:hideOthers];
            [appMenu addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
            [appMenu addItem:[NSMenuItem separatorItem]];

            NSString *quitTitle = [NSString stringWithFormat:@"Quit %@", appName];
            [appMenu addItemWithTitle:quitTitle action:@selector(terminate:) keyEquivalent:@"q"];
        }

        // 2. Build custom user submenus
        build_nsmenu_items(mainMenu, *items_to_build);

        for (NSMenuItem *topItem in mainMenu.itemArray) {
            if (!topItem.submenu) continue;
            if ([topItem.title isEqualToString:@"Window"] || [topItem.title isEqualToString:@"Finestra"]) {
                [NSApp setWindowsMenu:topItem.submenu];
            } else if ([topItem.title isEqualToString:@"Help"] || [topItem.title isEqualToString:@"Aiuto"]) {
                [NSApp setHelpMenu:topItem.submenu];
            }
        }

        [NSApp setMainMenu:mainMenu];
    });
#elif defined(_WIN32)
    g_last_menu_json = s_json;
    std::lock_guard<std::mutex> lock(g_windows_mutex);
    for (auto &pair : g_windows) {
        HWND hwnd = static_cast<HWND>(webview_get_window(pair.second.w));
        if (hwnd) {
            apply_menu_to_win32_window(hwnd, *items_to_build);
        }
    }
#else
    g_last_menu_json_linux = s_json;
    std::lock_guard<std::mutex> lock(g_windows_mutex);
    for (auto &pair : g_windows) {
        GtkWindow *gtkwin = GTK_WINDOW(webview_get_window(pair.second.w));
        if (gtkwin) {
            apply_menu_to_gtk_window(gtkwin, *items_to_build);
        }
    }
#endif
}

void platform_set_app_name(const char *name) {
    if (!name || name[0] == '\0') return;
#if defined(__APPLE__)
    platform_apply_app_name_macos(name);
#else
    g_app_name = name;
#endif
}

const char *platform_get_app_name() {
    return g_app_name.c_str();
}

int platform_is_origin_allowed(const char *url) {
    if (!url || url[0] == '\0') return 0;
    std::string dev_url_copy;
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        dev_url_copy = g_dev_url;
    }
    return OriginValidator::is_allowed(url, g_dev_mode.load(), dev_url_copy) ? 1 : 0;
}

int platform_resolve_path(const char *in_path, char *out_path, int max_len) {
    if (!in_path || in_path[0] == '\0' || !out_path || max_len <= 0) return 0;
    out_path[0] = '\0';
    try {
        std::filesystem::path current(in_path);
        std::error_code ec;
        for (int hop = 0; hop < 40; ++hop) {
            std::filesystem::file_status st = std::filesystem::symlink_status(current, ec);
            if (!ec && std::filesystem::is_symlink(st)) {
                std::filesystem::path target = std::filesystem::read_symlink(current, ec);
                if (ec) return 0;
                if (target.is_relative()) {
                    current = current.parent_path() / target;
                } else {
                    current = target;
                }
                continue;
            }
            ec.clear();
            break;
        }
        std::filesystem::file_status final_st = std::filesystem::symlink_status(current, ec);
        if (!ec && std::filesystem::is_symlink(final_st)) {
            return 0;
        }
        ec.clear();
        std::filesystem::path resolved;
        if (std::filesystem::exists(current, ec) && !ec) {
            resolved = std::filesystem::canonical(current, ec);
            if (ec) return 0;
        } else {
            ec.clear();
            std::vector<std::filesystem::path> tail;
            std::filesystem::path cur = current;
            while (!cur.empty() && cur != cur.root_path()) {
                std::filesystem::file_status cst = std::filesystem::symlink_status(cur, ec);
                if (!ec && std::filesystem::is_symlink(cst)) {
                    std::filesystem::path tgt = std::filesystem::read_symlink(cur, ec);
                    if (ec) return 0;
                    cur = tgt.is_relative() ? (cur.parent_path() / tgt) : tgt;
                    continue;
                }
                ec.clear();
                if (std::filesystem::exists(cur, ec) && !ec) {
                    if (!std::filesystem::is_directory(cur, ec) || ec) {
                        return 0;
                    }
                    break;
                }
                ec.clear();
                std::filesystem::path fname = cur.filename();
                if (fname == ".." || fname == ".") return 0;
                tail.push_back(fname);
                std::filesystem::path par = cur.parent_path();
                if (par == cur) break;
                cur = par;
            }
            std::filesystem::path base = cur.empty() ? std::filesystem::path(".") : cur;
            if (std::filesystem::exists(base, ec) && !ec) {
                resolved = std::filesystem::canonical(base, ec);
                if (ec) return 0;
            } else {
                ec.clear();
                resolved = std::filesystem::weakly_canonical(base, ec);
                if (ec) return 0;
            }
            for (auto it = tail.rbegin(); it != tail.rend(); ++it) {
                resolved /= *it;
            }
        }
        std::string s = resolved.generic_string();
        if (s.size() >= static_cast<size_t>(max_len)) return 0;
        memcpy(out_path, s.data(), s.size());
        out_path[s.size()] = '\0';
        return 1;
    } catch (...) {
        return 0;
    }
}

}