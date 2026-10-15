#include "native_bridge.h"
#include "origin_check.hpp"

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

#if defined(__APPLE__)
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#import <UserNotifications/UserNotifications.h>
#import <objc/runtime.h>
#endif

#define WEBVIEW_STATIC
#define WEBVIEW_IMPLEMENTATION
#include "webview.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl.h>
#include "WebView2.h"
using namespace Microsoft::WRL;
#elif !defined(__APPLE__)
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#endif

#if !defined(_WIN32)
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
extern char **environ;
#endif

static std::string g_csp_policy = "default-src 'self' tauri: asset:; script-src 'self' tauri: asset: 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src 'self' asset: data: tauri:; connect-src 'self' tauri: ipc: http://localhost:* ws://localhost:* http://127.0.0.1:* ws://127.0.0.1:* https://tauri.localhost;";

static std::string generate_session_token() {
    static const char chars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, sizeof(chars) - 2);
    std::string token;
    for (int i = 0; i < 32; ++i) {
        token += chars[dis(gen)];
    }
    return token;
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
    Json
};

struct StateValue {
    StateType type;
    std::string str_data;
    double num_data;
};

static std::unordered_map<std::string, StateValue> g_shared_state;
static std::shared_mutex g_state_rw_mutex;

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

static std::string get_mime(const std::string &path) {
    if (path.rfind(".html") != std::string::npos) return "text/html";
    if (path.rfind(".js") != std::string::npos) return "application/javascript";
    if (path.rfind(".css") != std::string::npos) return "text/css";
    if (path.rfind(".json") != std::string::npos) return "application/json";
    if (path.rfind(".png") != std::string::npos) return "image/png";
    if (path.rfind(".jpg") != std::string::npos || path.rfind(".jpeg") != std::string::npos) return "image/jpeg";
    if (path.rfind(".svg") != std::string::npos) return "image/svg+xml";
    if (path.rfind(".wasm") != std::string::npos) return "application/wasm";
    return "application/octet-stream";
}

static bool read_file_bytes(const std::string &full_path, std::string &out_data) {
    std::ifstream file(full_path, std::ios::binary);
    if (!file.is_open()) return false;
    std::ostringstream ss;
    ss << file.rdbuf();
    out_data = ss.str();
    return true;
}

#if defined(__APPLE__)
@interface TauriURLSchemeHandler : NSObject <WKURLSchemeHandler>
@end

@implementation TauriURLSchemeHandler
- (void)webView:(WKWebView *)webView startURLSchemeTask:(id<WKURLSchemeTask>)urlSchemeTask {
    NSURL *url = urlSchemeTask.request.URL;
    NSString *path = [url path];
    if ([path length] == 0 || [path isEqualToString:@"/"]) {
        path = @"/index.html";
    }

    std::string full_path = g_assets_root + [path UTF8String];
    std::string data;
    if (!read_file_bytes(full_path, data)) {
        NSString *resPath = [[NSBundle mainBundle] resourcePath];
        if (resPath) {
            std::string bundle_path = std::string([resPath UTF8String]) + "/" + g_assets_root + [path UTF8String];
            read_file_bytes(bundle_path, data);
        }
        if (data.empty()) {
            full_path = g_assets_root + "/index.html";
            if (!read_file_bytes(full_path, data) && resPath) {
                std::string bundle_fallback = std::string([resPath UTF8String]) + "/" + g_assets_root + "/index.html";
                read_file_bytes(bundle_fallback, data);
            }
        }
    }

    std::string mime = get_mime(full_path);
    if (mime == "text/html") {
        size_t head_pos = data.find("<head>");
        if (head_pos == std::string::npos) head_pos = data.find("<HEAD>");
        if (head_pos != std::string::npos && data.find("Content-Security-Policy") == std::string::npos) {
            std::string meta_tag = "\n  <meta http-equiv=\"Content-Security-Policy\" content=\"" + g_csp_policy + "\">";
            data.insert(head_pos + 6, meta_tag);
        }
    }

    NSURLResponse *response = [[NSHTTPURLResponse alloc] initWithURL:url
                                                          statusCode:200
                                                         HTTPVersion:@"HTTP/1.1"
                                                        headerFields:@{
                                                            @"Content-Type": [NSString stringWithUTF8String:mime.c_str()],
                                                            @"Access-Control-Allow-Origin": @"*",
                                                            @"Content-Security-Policy": [NSString stringWithUTF8String:g_csp_policy.c_str()]
                                                        }];

    [urlSchemeTask didReceiveResponse:response];
    NSData *nsdata = [NSData dataWithBytes:data.data() length:data.size()];
    [urlSchemeTask didReceiveData:nsdata];
    [urlSchemeTask didFinish];
}

- (void)webView:(WKWebView *)webView stopURLSchemeTask:(id<WKURLSchemeTask>)urlSchemeTask {
}
@end

static file_drop_callback_fn g_file_drop_cb = nullptr;
void platform_file_drop_set_callback(file_drop_callback_fn cb) {
    g_file_drop_cb = cb;
}

static void platform_dispatch_file_drop(const char *label, const char *event_type, const std::vector<std::string> &paths, double x, double y) {
    std::string json = "{\"paths\":[";
    for (size_t i = 0; i < paths.size(); ++i) {
        if (i > 0) json += ",";
        json += "\"";
        for (char c : paths[i]) {
            if (c == '"') json += "\\\"";
            else if (c == '\\') json += "\\\\";
            else json += c;
        }
        json += "\"";
    }
    json += "],\"position\":{\"x\":" + std::to_string((int)x) + ",\"y\":" + std::to_string((int)y) + "}}";

    if (g_file_drop_cb) {
        g_file_drop_cb(label ? label : "main", event_type ? event_type : "drop", json.c_str());
    }
}

@interface WKWebView (TauriDragDrop)
@end

@implementation WKWebView (TauriDragDrop)
+ (void)load {
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        Class cls = [WKWebView class];
        SEL origSel = @selector(performDragOperation:);
        SEL swizSel = @selector(tauri_performDragOperation:);
        Method origMethod = class_getInstanceMethod(cls, origSel);
        Method swizMethod = class_getInstanceMethod(cls, swizSel);
        if (origMethod && swizMethod) {
            method_exchangeImplementations(origMethod, swizMethod);
        }
    });
}

- (BOOL)tauri_performDragOperation:(id<NSDraggingInfo>)sender {
    NSPasteboard *pboard = [sender draggingPasteboard];
    NSArray *urls = [pboard readObjectsForClasses:@[[NSURL class]] options:nil];
    if (urls && [urls count] > 0) {
        std::vector<std::string> paths;
        for (NSURL *url in urls) {
            if ([url isFileURL]) {
                paths.push_back([[url path] UTF8String]);
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
    return [self tauri_performDragOperation:sender];
}
@end

@interface WKWebViewConfiguration (TauriSwizzle)
@end

@implementation WKWebViewConfiguration (TauriSwizzle)
+ (void)load {
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        Class cls = [self class];
        SEL origSel = @selector(init);
        SEL swizSel = @selector(tauri_init);
        Method origMethod = class_getInstanceMethod(cls, origSel);
        Method swizMethod = class_getInstanceMethod(cls, swizSel);
        method_exchangeImplementations(origMethod, swizMethod);
    });
}

- (instancetype)tauri_init {
    WKWebViewConfiguration *config = [self tauri_init];
    TauriURLSchemeHandler *handler = [[TauriURLSchemeHandler alloc] init];
    [config setURLSchemeHandler:handler forURLScheme:@"tauri"];
    [config setURLSchemeHandler:handler forURLScheme:@"asset"];
    return config;
}
@end
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
static void gtk_uri_scheme_request_cb(WebKitURISchemeRequest *request, gpointer user_data) {
    const gchar *path_str = webkit_uri_scheme_request_get_path(request);
    std::string path = (path_str && strlen(path_str) > 0) ? path_str : "/index.html";
    if (path == "/") path = "/index.html";

    std::string full_path = g_assets_root + path;
    std::string data;
    if (!read_file_bytes(full_path, data)) {
        full_path = g_assets_root + "/index.html";
        read_file_bytes(full_path, data);
    }

    std::string mime = get_mime(full_path);
    if (mime == "text/html") {
        size_t head_pos = data.find("<head>");
        if (head_pos == std::string::npos) head_pos = data.find("<HEAD>");
        if (head_pos != std::string::npos && data.find("Content-Security-Policy") == std::string::npos) {
            std::string meta_tag = "\n  <meta http-equiv=\"Content-Security-Policy\" content=\"" + g_csp_policy + "\">";
            data.insert(head_pos + 6, meta_tag);
        }
    }

    GInputStream *stream = g_memory_input_stream_new_from_data(data.data(), data.size(), NULL);
    webkit_uri_scheme_request_finish(request, stream, data.size(), mime.c_str());
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
            char buf[1024];
            WideCharToMultiByte(CP_UTF8, 0, uri_w, -1, buf, sizeof(buf), NULL, NULL);
            CoTaskMemFree(uri_w);
            core->Release();
            out_origin = std::string(buf);
            return true;
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
    GtkWidget *gtk_win = GTK_WIDGET(h);
    GList *children = gtk_container_get_children(GTK_CONTAINER(gtk_win));
    if (children) {
        GtkWidget *widget = GTK_WIDGET(children->data);
        g_list_free(children);
        if (WEBKIT_IS_WEB_VIEW(widget)) {
            const gchar *uri = webkit_web_view_get_uri(WEBKIT_WEB_VIEW(widget));
            if (uri) {
                out_origin = std::string(uri);
                return true;
            }
        }
    }
    return false;
#endif
}

extern "C" {

void platform_set_assets_path(const char *path) {
    if (path && strlen(path) > 0) {
        g_assets_root = path;
    }
}

static int g_dev_mode = 0;

void bridge_set_dev_mode(int dev) {
    g_dev_mode = dev;
}

void bridge_init(ipc_callback_fn on_ipc, tray_callback_fn on_tray, shortcut_callback_fn on_shortcut, deeplink_callback_fn on_deeplink) {
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
    std::lock_guard<std::mutex> lock(g_windows_mutex);
    auto it = g_windows.find(label);
    if (it != g_windows.end()) {
        ReturnPayload *p = new ReturnPayload();
        p->w = it->second.w;
        p->seq = seq ? seq : "";
        p->status = status;
        p->result = result ? result : "null";
        webview_dispatch(it->second.w, dispatch_return_cb, p);
    }
}

static void common_bind_handler(const char *seq, const char *req, void *arg) {
    const char *label_ptr = static_cast<const char *>(arg);
    std::string label = label_ptr ? label_ptr : "main";
    std::string s_seq = seq ? seq : "";
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
    if (!get_native_window_origin(target_w, current_origin) || !OriginValidator::is_allowed(current_origin)) {
        std::string err_payload = "{\"error\":\"Accesso negato: Origin non autorizzato (" + current_origin + ")\"}";
        platform_window_return_async(label.c_str(), s_seq.c_str(), 1, err_payload.c_str());
        return;
    }

    if (s_req.find("\"" + g_session_token + "\"") == std::string::npos) {
        std::string err_payload = "{\"error\":\"Accesso negato: Token di isolamento mancante o non valido\"}";
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

int platform_window_create(const char *label, const char *title, const char *url, int width, int height, int resizable, int decorations, int transparent) {
    std::lock_guard<std::mutex> lock(g_windows_mutex);
    std::string s_label = label ? label : "main";
    if (g_windows.find(s_label) != g_windows.end()) {
        return 0;
    }

    webview_t w = webview_create(g_dev_mode, nullptr);
    if (!w) return 0;

    webview_set_close_handler(w, on_internal_webview_close, strdup(s_label.c_str()));

    webview_set_title(w, title);
    webview_set_size(w, width, height, resizable ? 0 : 3);

    webview_init(w, get_isolation_bootstrap_script().c_str());

    char *label_ctx = strdup(s_label.c_str());
    webview_bind(w, "__TAURI_RAW_INVOKE__", common_bind_handler, label_ctx);

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
                wchar_t wroot[MAX_PATH];
                MultiByteToWideChar(CP_UTF8, 0, g_assets_root.c_str(), -1, wroot, MAX_PATH);
                core3->SetVirtualHostNameToFolderMapping(L"tauri.localhost", wroot, COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
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

    webview_navigate(w, url);
    g_windows[s_label] = WindowInstance{s_label, w};

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

    return 1;
}

void platform_window_close(const char *label) {
    std::lock_guard<std::mutex> lock(g_windows_mutex);
    auto it = g_windows.find(label ? label : "main");
    if (it != g_windows.end()) {
        void *h = webview_get_window(it->second.w);
#if defined(__APPLE__)
        run_on_main_async(^{
            [(__bridge NSWindow *)h performClose:nil];
        });
#elif defined(_WIN32)
        PostMessage(static_cast<HWND>(h), WM_CLOSE, 0, 0);
#else
        gtk_window_close(GTK_WINDOW(h));
#endif
    }
}

void platform_window_force_close(const char *label) {
    std::lock_guard<std::mutex> lock(g_windows_mutex);
    auto it = g_windows.find(label ? label : "main");
    if (it != g_windows.end()) {
        webview_force_close(it->second.w);
        g_windows.erase(it);
    }
}

void platform_window_set_title(const char *label, const char *title) {
    std::lock_guard<std::mutex> lock(g_windows_mutex);
    auto it = g_windows.find(label);
    if (it != g_windows.end()) {
        webview_set_title(it->second.w, title);
    }
}

void platform_window_set_size(const char *label, int width, int height) {
    std::lock_guard<std::mutex> lock(g_windows_mutex);
    auto it = g_windows.find(label);
    if (it != g_windows.end()) {
        webview_set_size(it->second.w, width, height, 0);
    }
}

void platform_window_minimize(const char *label) {
    std::lock_guard<std::mutex> lock(g_windows_mutex);
    auto it = g_windows.find(label);
    if (it == g_windows.end()) return;
    void *h = webview_get_window(it->second.w);
#if defined(_WIN32)
    ShowWindow(static_cast<HWND>(h), SW_MINIMIZE);
#elif defined(__APPLE__)
    run_on_main_sync(^{
        [(__bridge NSWindow *)h miniaturize:nil];
    });
#else
    gtk_window_iconify(GTK_WINDOW(h));
#endif
}

void platform_window_maximize(const char *label) {
    std::lock_guard<std::mutex> lock(g_windows_mutex);
    auto it = g_windows.find(label);
    if (it == g_windows.end()) return;
    void *h = webview_get_window(it->second.w);
#if defined(_WIN32)
    ShowWindow(static_cast<HWND>(h), SW_MAXIMIZE);
#elif defined(__APPLE__)
    run_on_main_sync(^{
        [(__bridge NSWindow *)h zoom:nil];
    });
#else
    gtk_window_maximize(GTK_WINDOW(h));
#endif
}

void platform_window_unmaximize(const char *label) {
    std::lock_guard<std::mutex> lock(g_windows_mutex);
    auto it = g_windows.find(label);
    if (it == g_windows.end()) return;
    void *h = webview_get_window(it->second.w);
#if defined(_WIN32)
    ShowWindow(static_cast<HWND>(h), SW_RESTORE);
#elif defined(__APPLE__)
    run_on_main_sync(^{
        [(__bridge NSWindow *)h zoom:nil];
    });
#else
    gtk_window_unmaximize(GTK_WINDOW(h));
#endif
}

void platform_window_show(const char *label) {
    std::lock_guard<std::mutex> lock(g_windows_mutex);
    auto it = g_windows.find(label);
    if (it == g_windows.end()) return;
    void *h = webview_get_window(it->second.w);
#if defined(_WIN32)
    ShowWindow(static_cast<HWND>(h), SW_SHOW);
#elif defined(__APPLE__)
    run_on_main_sync(^{
        [(__bridge NSWindow *)h makeKeyAndOrderFront:nil];
    });
#else
    gtk_widget_show_all(GTK_WIDGET(h));
#endif
}

void platform_window_hide(const char *label) {
    std::lock_guard<std::mutex> lock(g_windows_mutex);
    auto it = g_windows.find(label);
    if (it == g_windows.end()) return;
    void *h = webview_get_window(it->second.w);
#if defined(_WIN32)
    ShowWindow(static_cast<HWND>(h), SW_HIDE);
#elif defined(__APPLE__)
    run_on_main_sync(^{
        [(__bridge NSWindow *)h orderOut:nil];
    });
#else
    gtk_widget_hide(GTK_WIDGET(h));
#endif
}

void platform_window_set_focus(const char *label) {
    std::lock_guard<std::mutex> lock(g_windows_mutex);
    auto it = g_windows.find(label ? label : "main");
    if (it == g_windows.end() && !g_windows.empty()) {
        it = g_windows.begin();
    }
    if (it == g_windows.end()) return;
    void *h = webview_get_window(it->second.w);
#if defined(_WIN32)
    HWND hwnd = static_cast<HWND>(h);
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
#elif defined(__APPLE__)
    run_on_main_sync(^{
        NSWindow *win = (__bridge NSWindow *)h;
        [NSApp activateIgnoringOtherApps:YES];
        if ([win isMiniaturized]) {
            [win deminiaturize:nil];
        }
        [win makeKeyAndOrderFront:nil];
    });
#else
    gtk_window_deiconify(GTK_WINDOW(h));
    gtk_window_present(GTK_WINDOW(h));
#endif
}

void platform_window_start_dragging(const char *label) {
    std::lock_guard<std::mutex> lock(g_windows_mutex);
    auto it = g_windows.find(label);
    if (it == g_windows.end()) return;
    void *h = webview_get_window(it->second.w);
#if defined(_WIN32)
    HWND hwnd = static_cast<HWND>(h);
    ReleaseCapture();
    SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
#elif defined(__APPLE__)
    run_on_main_sync(^{
        NSWindow *nswin = (__bridge NSWindow *)h;
        NSEvent *event = [NSApp currentEvent];
        if (event) {
            [nswin performWindowDragWithEvent:event];
        }
    });
#else
    GtkWindow *gtkwin = GTK_WINDOW(h);
    gtk_window_begin_move_drag(gtkwin, 1, 0, 0, GDK_CURRENT_TIME);
#endif
}

struct EvalPayload {
    webview_t w;
    std::string js;
};

static void dispatch_eval_cb(webview_t w, void *arg) {
    EvalPayload *p = static_cast<EvalPayload *>(arg);
    webview_eval(p->w, p->js.c_str());
    delete p;
}

void platform_window_eval(const char *label, const char *js) {
    if (!label || !js) return;
    std::lock_guard<std::mutex> lock(g_windows_mutex);
    if (strcmp(label, "*") == 0) {
        for (auto &pair : g_windows) {
            EvalPayload *p = new EvalPayload();
            p->w = pair.second.w;
            p->js = js;
            webview_dispatch(pair.second.w, dispatch_eval_cb, p);
        }
        return;
    }
    auto it = g_windows.find(label);
    if (it != g_windows.end()) {
        if (strstr(js, "window.__TAURI__") != nullptr) {
            webview_init(it->second.w, js);
        }
        EvalPayload *p = new EvalPayload();
        p->w = it->second.w;
        p->js = js;
        webview_dispatch(it->second.w, dispatch_eval_cb, p);
    }
}

void bridge_run_loop() {
    if (!g_windows.empty()) {
        webview_run(g_windows.begin()->second.w);
    }
}

void bridge_terminate() {
    platform_single_instance_cleanup();
    std::lock_guard<std::mutex> lock(g_windows_mutex);
    for (auto &pair : g_windows) {
        webview_terminate(pair.second.w);
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
        g_status_item.button.title = [NSString stringWithUTF8String:s_tip.c_str()];
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
    AppendMenuA(g_tray_menu, MF_STRING, 1, "Apri Finestra");
    AppendMenuA(g_tray_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(g_tray_menu, MF_STRING, 2, "Esci");
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
#if defined(_WIN32)
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    std::string key = "Software\\Classes\\" + std::string(scheme);
    HKEY hKey;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, key.c_str(), 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        std::string desc = "URL:" + std::string(scheme) + " Protocol";
        RegSetValueExA(hKey, "", 0, REG_SZ, (const BYTE *)desc.c_str(), (DWORD)desc.size() + 1);
        std::string cmd = "\"" + std::string(exe_path) + "\" \"%1\"";
        HKEY hCmd;
        RegCreateKeyExA(hKey, "shell\\open\\command", 0, NULL, 0, KEY_WRITE, NULL, &hCmd, NULL);
        RegSetValueExA(hCmd, "", 0, REG_SZ, (const BYTE *)cmd.c_str(), (DWORD)cmd.size() + 1);
        RegCloseKey(hCmd);
        RegCloseKey(hKey);
        return 1;
    }
    return 0;
#elif defined(__APPLE__)
    return 1;
#else
    return 1;
#endif
}

int platform_updater_install(const char *archive_path) {
#if defined(_WIN32)
    std::string cmd = "tar -xf \"" + std::string(archive_path) + "\" -C .";
    return system(cmd.c_str()) == 0 ? 1 : 0;
#else
    std::string cmd = "tar -xzf \"" + std::string(archive_path) + "\" -C .";
    return system(cmd.c_str()) == 0 ? 1 : 0;
#endif
}

int platform_dialog_open(const char *title, const char *filter, char *out_path, int max_len) {
#if defined(_WIN32)
    OPENFILENAMEA ofn;
    char szFile[MAX_PATH] = {0};
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = filter ? filter : "All Files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) {
        strncpy(out_path, ofn.lpstrFile, max_len);
        return 1;
    }
    return 0;
#elif defined(__APPLE__)
    __block int res = 0;
    std::string s_title = title ? title : "";
    run_on_main_sync(^{
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];
        if (!s_title.empty()) [panel setTitle:[NSString stringWithUTF8String:s_title.c_str()]];
        if ([panel runModal] == NSModalResponseOK) {
            NSURL *url = [[panel URLs] objectAtIndex:0];
            strncpy(out_path, [[url path] UTF8String], max_len);
            res = 1;
        }
    });
    return res;
#else
    GtkWidget *dialog = gtk_file_chooser_dialog_new(title ? title : "Open File", NULL, GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
    int res = 0;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        strncpy(out_path, filename, max_len);
        g_free(filename);
        res = 1;
    }
    gtk_widget_destroy(dialog);
    while (g_main_context_iteration(NULL, FALSE));
    return res;
#endif
}

int platform_dialog_save(const char *title, const char *filter, char *out_path, int max_len) {
#if defined(_WIN32)
    OPENFILENAMEA ofn;
    char szFile[MAX_PATH] = {0};
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = filter ? filter : "All Files\0*.*\0";
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameA(&ofn)) {
        strncpy(out_path, ofn.lpstrFile, max_len);
        return 1;
    }
    return 0;
#elif defined(__APPLE__)
    __block int res = 0;
    std::string s_title = title ? title : "";
    run_on_main_sync(^{
        NSSavePanel *panel = [NSSavePanel savePanel];
        if (!s_title.empty()) [panel setTitle:[NSString stringWithUTF8String:s_title.c_str()]];
        if ([panel runModal] == NSModalResponseOK) {
            strncpy(out_path, [[[panel URL] path] UTF8String], max_len);
            res = 1;
        }
    });
    return res;
#else
    GtkWidget *dialog = gtk_file_chooser_dialog_new(title ? title : "Save File", NULL, GTK_FILE_CHOOSER_ACTION_SAVE, "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
    int res = 0;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        strncpy(out_path, filename, max_len);
        g_free(filename);
        res = 1;
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
        [alert setMessageText:[NSString stringWithUTF8String:s_title.c_str()]];
        [alert setInformativeText:[NSString stringWithUTF8String:s_msg.c_str()]];
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
        [pb setString:[NSString stringWithUTF8String:s_text.c_str()] forType:NSPasteboardTypeString];
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
#if defined(_WIN32)
    if (!OpenClipboard(NULL)) return 0;
    HANDLE hData = GetClipboardData(CF_TEXT);
    if (!hData) {
        CloseClipboard();
        return 0;
    }
    char *pszText = static_cast<char *>(GlobalLock(hData));
    if (pszText) {
        strncpy(out_text, pszText, max_len);
        GlobalUnlock(hData);
        CloseClipboard();
        return 1;
    }
    CloseClipboard();
    return 0;
#elif defined(__APPLE__)
    __block int ok = 0;
    run_on_main_sync(^{
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        NSString *str = [pb stringForType:NSPasteboardTypeString];
        if (str) {
            strncpy(out_text, [str UTF8String], max_len);
            ok = 1;
        }
    });
    return ok;
#else
    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gchar *text = gtk_clipboard_wait_for_text(clipboard);
    if (text) {
        strncpy(out_text, text, max_len);
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
        [pb setString:[NSString stringWithUTF8String:s_html.c_str()] forType:NSPasteboardTypeHTML];
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
            strncpy(out_html, [str UTF8String], max_len - 1);
            out_html[max_len - 1] = '\0';
            ok = 1;
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

static theme_change_callback_fn g_theme_cb = nullptr;
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

void platform_theme_set_callback(theme_change_callback_fn cb) {
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

void platform_notification_send(const char *title, const char *body) {
    if (!title && !body) return;
    std::string s_title = title ? title : "";
    std::string s_body = body ? body : "";

#if defined(__APPLE__)
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
                content.title = [NSString stringWithUTF8String:s_title.c_str()];
                content.body = [NSString stringWithUTF8String:s_body.c_str()];
                content.sound = [UNNotificationSound defaultSound];

                NSString *identifier = [[NSUUID UUID] UUIDString];
                UNNotificationRequest *request = [UNNotificationRequest requestWithIdentifier:identifier content:content trigger:nil];
                [[UNUserNotificationCenter currentNotificationCenter] addNotificationRequest:request withCompletionHandler:nil];
            } else {
                std::string script = "display notification \"" + s_body + "\" with title \"" + s_title + "\"";
                pid_t pid;
                const char *argv[] = {"osascript", "-e", script.c_str(), NULL};
                if (posix_spawnp(&pid, "/usr/bin/osascript", NULL, NULL, (char *const *)argv, environ) == 0) {
                    int status;
                    waitpid(pid, &status, 0);
                }
            }
        });
        return;
    }
    // Terminal fallback for dev mode: discrete non-blocking invocation
    std::thread([s_title, s_body]() {
        std::string script = "display notification \"" + s_body + "\" with title \"" + s_title + "\"";
        pid_t pid;
        const char *argv[] = {"osascript", "-e", script.c_str(), NULL};
        if (posix_spawnp(&pid, "/usr/bin/osascript", NULL, NULL, (char *const *)argv, environ) == 0) {
            int status;
            waitpid(pid, &status, 0);
        }
    }).detach();

#elif defined(_WIN32)
    std::thread([s_title, s_body]() {
        std::string ps = 
            "[Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime] > $null; "
            "$template = [Windows.UI.Notifications.ToastNotificationManager]::GetTemplateContent([Windows.UI.Notifications.ToastTemplateType]::ToastText02); "
            "$textNodes = $template.GetElementsByTagName('text'); "
            "$textNodes.Item(0).AppendChild($template.CreateTextNode('" + s_title + "')) > $null; "
            "$textNodes.Item(1).AppendChild($template.CreateTextNode('" + s_body + "')) > $null; "
            "$toast = [Windows.UI.Notifications.ToastNotification]::new($template); "
            "[Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier('Tauri').Show($toast);";

        std::string cmd = "powershell -NoProfile -NonInteractive -WindowStyle Hidden -Command \"" + ps + "\"";
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        ZeroMemory(&pi, sizeof(pi));
        if (CreateProcessA(NULL, (LPSTR)cmd.c_str(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }).detach();

#else
    std::thread([s_title, s_body]() {
        pid_t pid;
        const char *argv[] = {"notify-send", s_title.c_str(), s_body.c_str(), NULL};
        if (posix_spawnp(&pid, "notify-send", NULL, NULL, (char *const *)argv, environ) == 0) {
            int status;
            waitpid(pid, &status, 0);
        }
    }).detach();
#endif
}

static single_instance_callback_fn g_single_instance_cb = nullptr;
static std::thread g_single_instance_thread;
static std::atomic<bool> g_single_instance_running{false};
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
        close(g_single_instance_server_fd);
        g_single_instance_server_fd = -1;
    }
    if (!g_single_instance_sock_path.empty()) {
        unlink(g_single_instance_sock_path.c_str());
        g_single_instance_sock_path.clear();
    }
#else
    if (g_single_instance_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(g_single_instance_pipe);
        g_single_instance_pipe = INVALID_HANDLE_VALUE;
    }
#endif
}

int platform_single_instance_init(const char *app_id, const char *payload_json) {
    static bool g_single_instance_initialized = false;
    if (g_single_instance_initialized) return 1;

    std::string safe_id = (app_id && strlen(app_id) > 0) ? app_id : "com.tauri.lua";
    for (char &c : safe_id) {
        if (!isalnum((unsigned char)c) && c != '-' && c != '.' && c != '_') {
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
        sock_path = "/tmp/" + safe_id + ".sock";
    }

    // Try connecting as a secondary instance first
    int client_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client_sock >= 0) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 300000; // 300ms timeout
        setsockopt(client_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (connect(client_sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            // Secondary instance: connected to active primary instance!
            std::string msg = payload + "\n";
            send(client_sock, msg.c_str(), msg.length(), 0);
            close(client_sock);
            return 0; // Signal: secondary instance, should exit immediately
        }
        close(client_sock);
    }

    // If connect failed, remove any stale dead socket file
    unlink(sock_path.c_str());

    // Become primary instance server
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) return -1;

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_un saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sun_family = AF_UNIX;
    strncpy(saddr.sun_path, sock_path.c_str(), sizeof(saddr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        close(server_fd);
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
        while (g_single_instance_running) {
            struct sockaddr_un client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
            if (client_fd < 0) {
                if (!g_single_instance_running) break;
                continue;
            }
            std::string buffer;
            char chunk[512];
            ssize_t bytes_read = 0;
            while ((bytes_read = recv(client_fd, chunk, sizeof(chunk) - 1, 0)) > 0) {
                chunk[bytes_read] = '\0';
                buffer += chunk;
                if (buffer.find('\n') != std::string::npos) break;
            }
            close(client_fd);

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
    g_single_instance_thread.detach();

    return 1; // Primary instance initialized!

#else // Windows
    std::string pipe_name = "\\\\.\\pipe\\" + safe_id + "-single-instance";

    HANDLE hPipe = CreateFileA(pipe_name.c_str(), GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hPipe != INVALID_HANDLE_VALUE) {
        std::string msg = payload + "\n";
        DWORD bytesWritten = 0;
        WriteFile(hPipe, msg.c_str(), (DWORD)msg.length(), &bytesWritten, NULL);
        CloseHandle(hPipe);
        return 0; // Secondary instance!
    }

    HANDLE server_pipe = CreateNamedPipeA(
        pipe_name.c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        4096, 4096, 0, NULL
    );
    if (server_pipe == INVALID_HANDLE_VALUE) return -1;

    g_single_instance_pipe = server_pipe;
    g_single_instance_running = true;
    g_single_instance_initialized = true;

    g_single_instance_thread = std::thread([pipe_name]() {
        while (g_single_instance_running) {
            HANDLE hInstPipe = CreateNamedPipeA(
                pipe_name.c_str(),
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                4096, 4096, 0, NULL
            );
            if (hInstPipe == INVALID_HANDLE_VALUE) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
            BOOL connected = ConnectNamedPipe(hInstPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
            if (connected) {
                char buf[4096];
                DWORD bytesRead = 0;
                std::string buffer;
                if (ReadFile(hInstPipe, buf, sizeof(buf) - 1, &bytesRead, NULL) && bytesRead > 0) {
                    buf[bytesRead] = '\0';
                    buffer = buf;
                }
                DisconnectNamedPipe(hInstPipe);
                CloseHandle(hInstPipe);

                while (!buffer.empty() && (buffer.back() == '\n' || buffer.back() == '\r')) {
                    buffer.pop_back();
                }
                if (buffer.empty()) buffer = "{}";

                platform_window_set_focus("main");
                if (g_single_instance_cb) {
                    g_single_instance_cb(buffer.c_str());
                }
            } else {
                CloseHandle(hInstPipe);
            }
        }
    });
    g_single_instance_thread.detach();

    return 1;
#endif
}

void platform_state_set_string(const char *key, const char *val) {
    if (!key || !val) return;
    std::unique_lock<std::shared_mutex> lock(g_state_rw_mutex);
    g_shared_state[key] = StateValue{StateType::String, val, 0.0};
}

int platform_state_get_string(const char *key, char *out_val, int max_len) {
    if (!key || !out_val || max_len <= 0) return 0;
    std::shared_lock<std::shared_mutex> lock(g_state_rw_mutex);
    auto it = g_shared_state.find(key);
    if (it != g_shared_state.end() && it->second.type == StateType::String) {
        strncpy(out_val, it->second.str_data.c_str(), max_len - 1);
        out_val[max_len - 1] = '\0';
        return 1;
    }
    return 0;
}

void platform_state_set_number(const char *key, double val) {
    if (!key) return;
    std::unique_lock<std::shared_mutex> lock(g_state_rw_mutex);
    g_shared_state[key] = StateValue{StateType::Number, "", val};
}

int platform_state_get_number(const char *key, double *out_val) {
    if (!key || !out_val) return 0;
    std::shared_lock<std::shared_mutex> lock(g_state_rw_mutex);
    auto it = g_shared_state.find(key);
    if (it != g_shared_state.end() && it->second.type == StateType::Number) {
        *out_val = it->second.num_data;
        return 1;
    }
    return 0;
}

void platform_state_set_json(const char *key, const char *json_val) {
    if (!key || !json_val) return;
    std::unique_lock<std::shared_mutex> lock(g_state_rw_mutex);
    g_shared_state[key] = StateValue{StateType::Json, json_val, 0.0};
}

int platform_state_get_json(const char *key, char *out_val, int max_len) {
    if (!key || !out_val || max_len <= 0) return 0;
    std::shared_lock<std::shared_mutex> lock(g_state_rw_mutex);
    auto it = g_shared_state.find(key);
    if (it != g_shared_state.end() && it->second.type == StateType::Json) {
        strncpy(out_val, it->second.str_data.c_str(), max_len - 1);
        out_val[max_len - 1] = '\0';
        return 1;
    }
    return 0;
}

int platform_state_has(const char *key) {
    if (!key) return 0;
    std::shared_lock<std::shared_mutex> lock(g_state_rw_mutex);
    return g_shared_state.find(key) != g_shared_state.end() ? 1 : 0;
}

void platform_state_delete(const char *key) {
    if (!key) return;
    std::unique_lock<std::shared_mutex> lock(g_state_rw_mutex);
    g_shared_state.erase(key);
}

void platform_state_clear() {
    std::unique_lock<std::shared_mutex> lock(g_state_rw_mutex);
    g_shared_state.clear();
}

void platform_set_csp(const char *csp) {
    if (csp && strlen(csp) > 0) {
        g_csp_policy = csp;
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

    std::string cmd_line;
    auto append_quoted = [&](const std::string &arg) {
        if (!cmd_line.empty()) cmd_line += " ";
        if (arg.find(' ') != std::string::npos || arg.find('\t') != std::string::npos || arg.find('\"') != std::string::npos || arg.empty()) {
            cmd_line += "\"";
            for (char c : arg) {
                if (c == '\"') cmd_line += "\\\"";
                else cmd_line += c;
            }
            cmd_line += "\"";
        } else {
            cmd_line += arg;
        }
    };

    append_quoted(program);
    for (int i = 0; i < arg_count; ++i) {
        if (args && args[i]) {
            append_quoted(args[i]);
        }
    }

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdError = hWrite;
    si.hStdOutput = hWrite;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    std::vector<char> cmd_buf(cmd_line.begin(), cmd_line.end());
    cmd_buf.push_back('\0');

    BOOL success = CreateProcessA(
        NULL,
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
    if (pipe(pipe_fds) != 0) {
        return nullptr;
    }

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

    pid_t pid = -1;
    int status = posix_spawnp(&pid, program, &actions, nullptr, argv.data(), environ);
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
            return 0;
        }
#else
        char temp_buf[4096];
        ssize_t bytes_read = read(proc->pipe_fd, temp_buf, sizeof(temp_buf));
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
            return 0;
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
        waitpid(proc->pid, &status, 0);
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

void platform_menu_set_callback(menu_callback_fn on_menu) {
    g_menu_cb = on_menu;
}

void platform_window_set_close_callback(window_close_callback_fn on_close) {
    g_window_close_cb = on_close;
}

struct JsonVal {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JsonVal> arr;
    std::map<std::string, JsonVal> obj;

    const JsonVal &operator[](const std::string &k) const {
        static JsonVal null_val;
        auto it = obj.find(k);
        return it != obj.end() ? it->second : null_val;
    }
    std::string str(const std::string &def = "") const {
        return type == String ? s : def;
    }
    bool boolean(bool def = false) const {
        return type == Bool ? b : def;
    }
};

static void skip_ws(const std::string &s, size_t &pos) {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\r' || s[pos] == '\n')) {
        pos++;
    }
}

static JsonVal parse_json_val(const std::string &s, size_t &pos);

static std::string parse_json_str(const std::string &s, size_t &pos) {
    std::string res;
    if (pos >= s.size() || s[pos] != '"') return res;
    pos++;
    while (pos < s.size()) {
        char c = s[pos++];
        if (c == '"') break;
        if (c == '\\' && pos < s.size()) {
            char esc = s[pos++];
            if (esc == '"') res += '"';
            else if (esc == '\\') res += '\\';
            else if (esc == 'n') res += '\n';
            else if (esc == 'r') res += '\r';
            else if (esc == 't') res += '\t';
            else res += esc;
        } else {
            res += c;
        }
    }
    return res;
}

static JsonVal parse_json_val(const std::string &s, size_t &pos) {
    JsonVal v;
    skip_ws(s, pos);
    if (pos >= s.size()) return v;

    if (s[pos] == '"') {
        v.type = JsonVal::String;
        v.s = parse_json_str(s, pos);
    } else if (s[pos] == '{') {
        v.type = JsonVal::Object;
        pos++;
        skip_ws(s, pos);
        while (pos < s.size() && s[pos] != '}') {
            skip_ws(s, pos);
            std::string key = parse_json_str(s, pos);
            skip_ws(s, pos);
            if (pos < s.size() && s[pos] == ':') pos++;
            v.obj[key] = parse_json_val(s, pos);
            skip_ws(s, pos);
            if (pos < s.size() && s[pos] == ',') pos++;
            skip_ws(s, pos);
        }
        if (pos < s.size() && s[pos] == '}') pos++;
    } else if (s[pos] == '[') {
        v.type = JsonVal::Array;
        pos++;
        skip_ws(s, pos);
        while (pos < s.size() && s[pos] != ']') {
            v.arr.push_back(parse_json_val(s, pos));
            skip_ws(s, pos);
            if (pos < s.size() && s[pos] == ',') pos++;
            skip_ws(s, pos);
        }
        if (pos < s.size() && s[pos] == ']') pos++;
    } else if (s.compare(pos, 4, "true") == 0) {
        v.type = JsonVal::Bool; v.b = true; pos += 4;
    } else if (s.compare(pos, 5, "false") == 0) {
        v.type = JsonVal::Bool; v.b = false; pos += 5;
    } else if (s.compare(pos, 4, "null") == 0) {
        v.type = JsonVal::Null; pos += 4;
    } else {
        size_t start = pos;
        if (pos < s.size() && (s[pos] == '-' || s[pos] == '+')) pos++;
        while (pos < s.size() && ((s[pos] >= '0' && s[pos] <= '9') || s[pos] == '.')) pos++;
        v.type = JsonVal::Number;
        try { v.n = std::stod(s.substr(start, pos - start)); } catch (...) { v.n = 0; }
    }
    return v;
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

    if (accel.find("Shift") != std::string::npos) {
        *out_flags |= NSEventModifierFlagShift;
    }
    if (accel.find("CmdOrCtrl") != std::string::npos || accel.find("Command") != std::string::npos || accel.find("Cmd") != std::string::npos) {
        *out_flags |= NSEventModifierFlagCommand;
    }
    if (accel.find("Alt") != std::string::npos || accel.find("Option") != std::string::npos) {
        *out_flags |= NSEventModifierFlagOption;
    }
    if (accel.find("Control") != std::string::npos || accel.find("Ctrl") != std::string::npos) {
        *out_flags |= NSEventModifierFlagControl;
    }

    size_t last_plus = accel.rfind('+');
    std::string key = (last_plus != std::string::npos) ? accel.substr(last_plus + 1) : accel;
    if (!key.empty()) {
        std::string lower_key = key;
        for (char &c : lower_key) c = tolower(c);
        *out_key = [NSString stringWithUTF8String:lower_key.c_str()];
    }
}

static void build_nsmenu_items(NSMenu *menu, const std::vector<JsonVal> &items) {
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
            NSString *nsTitle = [NSString stringWithUTF8String:label.c_str()];
            NSMenuItem *subParent = [[NSMenuItem alloc] initWithTitle:nsTitle action:nil keyEquivalent:@""];
            NSMenu *sub = [[NSMenu alloc] initWithTitle:nsTitle];
            build_nsmenu_items(sub, item["items"].arr);
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
                keyEq = @"Z";
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
            NSString *nsTitle = [NSString stringWithUTF8String:label.c_str()];
            NSMenuItem *mi = [[NSMenuItem alloc] initWithTitle:nsTitle action:action keyEquivalent:keyEq];
            if (modFlags != 0) {
                [mi setKeyEquivalentModifierMask:modFlags];
            }
            [menu addItem:mi];
        } else {
            parse_accelerator(accel, &keyEq, &modFlags);
            NSString *nsTitle = [NSString stringWithUTF8String:label.c_str()];
            NSMenuItem *mi = [[NSMenuItem alloc] initWithTitle:nsTitle action:@selector(menuItemClicked:) keyEquivalent:keyEq];
            if (modFlags != 0) {
                [mi setKeyEquivalentModifierMask:modFlags];
            }
            [mi setTarget:[TauriMenuHandler sharedInstance]];
            mi.representedObject = [NSString stringWithUTF8String:id.c_str()];
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
        auto it = g_win32_menu_id_to_action.find(cmd);
        if (it != g_win32_menu_id_to_action.end()) {
            std::string action = it->second;
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
                // Native system role
            } else {
                if (g_menu_cb) {
                    g_menu_cb(action.c_str());
                }
            }
            return 0;
        }
    }
    auto proc_it = g_original_wndprocs.find(hwnd);
    if (proc_it != g_original_wndprocs.end() && proc_it->second) {
        return CallWindowProcW(proc_it->second, hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void build_win32_menu_items(HMENU parent, const std::vector<JsonVal> &items) {
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
            build_win32_menu_items(hSub, item["items"].arr);
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
    size_t pos = 0;
    JsonVal root = parse_json_val(g_last_menu_json, pos);
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

static void build_gtk_menu_items(GtkWidget *parent_shell, const std::vector<JsonVal> &items, GtkAccelGroup *accel_group) {
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
            build_gtk_menu_items(sub_menu, item["items"].arr, accel_group);
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
        g_signal_connect_data(item_widget, "activate", G_CALLBACK(on_gtk_menu_item_activate), strdup(action.c_str()), (GClosureNotify)free, (GConnectFlags)0);

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
    size_t pos = 0;
    JsonVal root = parse_json_val(g_last_menu_json_linux, pos);
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
    size_t pos = 0;
    JsonVal root = parse_json_val(s_json, pos);
    const std::vector<JsonVal> *items_to_build = nullptr;
    if (root.type == JsonVal::Array) {
        items_to_build = &root.arr;
    } else if (root.type == JsonVal::Object && root.obj.count("items") && root.obj["items"].type == JsonVal::Array) {
        items_to_build = &root.obj["items"].arr;
    }
    if (!items_to_build) return;

#if defined(__APPLE__)
    run_on_main_sync(^{
        NSString *appName = [[[NSBundle mainBundle] infoDictionary] objectForKey:@"CFBundleName"];
        if (!appName || [appName length] == 0) {
            appName = [[NSProcessInfo processInfo] processName];
        }
        if (!appName || [appName length] == 0) {
            appName = @"Tauri App";
        }

        NSMenu *mainMenu = [[NSMenu alloc] init];

        // 1. Build default App Menu
        NSMenuItem *appMenuItem = [[NSMenuItem alloc] init];
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

        // 2. Build custom user submenus
        build_nsmenu_items(mainMenu, *items_to_build);

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

}