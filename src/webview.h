#ifndef WEBVIEW_H
#define WEBVIEW_H

#include <string>
#include <map>
#include <functional>
#include <cstring>
#include "security_policy.hpp"
#include "ipc_envelope.hpp"

extern int g_dev_mode;
extern std::string g_dev_url;

typedef void *webview_t;

#define WEBVIEW_NATIVE_HANDLE_KIND_UI_WINDOW 0
#define WEBVIEW_NATIVE_HANDLE_KIND_UI_WIDGET 1
#define WEBVIEW_NATIVE_HANDLE_KIND_BROWSER_CONTROLLER 2

typedef void (*webview_bind_fn)(const char *seq, const char *req, void *arg);
typedef void (*webview_dispatch_fn)(webview_t w, void *arg);
typedef int (*webview_close_cb_fn)(webview_t w, void *arg);

struct WebviewBinding {
    webview_bind_fn fn;
    void *arg;
};

static std::string webview_js_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += c; break;
        }
    }
    return out;
}

static std::string g_app_name = "";

#if defined(__APPLE__)
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#import <dispatch/dispatch.h>

struct WebviewImpl {
    NSWindow *window;
    WKWebView *wk;
    WKUserContentController *ucc;
    id handler;
    bool is_main;
    std::map<std::string, WebviewBinding> bindings;
    webview_close_cb_fn close_cb = nullptr;
    void *close_arg = nullptr;
    bool force_closing = false;
};

static bool g_has_main_window = false;

@interface TauriBridgeScriptHandler : NSObject <WKScriptMessageHandler, NSWindowDelegate, WKNavigationDelegate, WKUIDelegate>
@property (nonatomic, assign) WebviewImpl *impl;
@end

@implementation TauriBridgeScriptHandler
- (void)userContentController:(WKUserContentController *)userContentController
      didReceiveScriptMessage:(WKScriptMessage *)message {
    if (!message.frameInfo.isMainFrame) return; // REJECT SUBFRAMES!
    if (!self.impl) return;
    if (![message.body isKindOfClass:[NSDictionary class]]) return;
    NSDictionary *dict = (NSDictionary *)message.body;
    NSString *name = dict[@"name"];
    NSString *seq = dict[@"seq"];
    NSString *req = dict[@"req"];
    if (!name || !seq || !req) return;

    auto it = self.impl->bindings.find([name UTF8String]);
    if (it != self.impl->bindings.end() && it->second.fn) {
        it->second.fn([seq UTF8String], [req UTF8String], it->second.arg);
    }
}

- (void)webView:(WKWebView *)webView decidePolicyForNavigationAction:(WKNavigationAction *)navigationAction decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler {
    NSURL *url = navigationAction.request.URL;
    std::string s_url = url ? [[url absoluteString] UTF8String] : "";
    if (SecurityPolicy::is_navigation_allowed(s_url, g_dev_mode != 0, g_dev_url)) {
        decisionHandler(WKNavigationActionPolicyAllow);
    } else {
        decisionHandler(WKNavigationActionPolicyCancel);
    }
}

- (WKWebView *)webView:(WKWebView *)webView createWebViewWithConfiguration:(WKWebViewConfiguration *)configuration forNavigationAction:(WKNavigationAction *)navigationAction windowFeatures:(WKWindowFeatures *)windowFeatures {
    return nil; // Block unexpected popup windows
}

- (void)webView:(WKWebView *)webView runJavaScriptAlertPanelWithMessage:(NSString *)message initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(void))completionHandler {
    NSAlert *alert = [[NSAlert alloc] init];
    [alert setMessageText:[[NSProcessInfo processInfo] processName]];
    [alert setInformativeText:message ? message : @""];
    [alert addButtonWithTitle:@"OK"];
    [NSApp activateIgnoringOtherApps:YES];
    [alert runModal];
    completionHandler();
}

- (void)webView:(WKWebView *)webView runJavaScriptConfirmPanelWithMessage:(NSString *)message initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(BOOL result))completionHandler {
    NSAlert *alert = [[NSAlert alloc] init];
    [alert setMessageText:[[NSProcessInfo processInfo] processName]];
    [alert setInformativeText:message ? message : @""];
    [alert addButtonWithTitle:@"OK"];
    [alert addButtonWithTitle:@"Annulla"];
    if ([alert.buttons count] > 1) {
        [[alert.buttons objectAtIndex:1] setKeyEquivalent:@"\033"];
    }
    [NSApp activateIgnoringOtherApps:YES];
    NSModalResponse res = [alert runModal];
    completionHandler(res == NSAlertFirstButtonReturn);
}

- (void)webView:(WKWebView *)webView runJavaScriptTextInputPanelWithPrompt:(NSString *)prompt defaultText:(NSString *)defaultText initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(NSString * _Nullable result))completionHandler {
    NSAlert *alert = [[NSAlert alloc] init];
    [alert setMessageText:[[NSProcessInfo processInfo] processName]];
    [alert setInformativeText:prompt ? prompt : @""];
    [alert addButtonWithTitle:@"OK"];
    [alert addButtonWithTitle:@"Annulla"];
    if ([alert.buttons count] > 1) {
        [[alert.buttons objectAtIndex:1] setKeyEquivalent:@"\033"];
    }
    NSTextField *input = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 300, 24)];
    if (defaultText) [input setStringValue:defaultText];
    [alert setAccessoryView:input];
    [alert layout];
    [alert.window setInitialFirstResponder:input];
    [NSApp activateIgnoringOtherApps:YES];
    NSModalResponse res = [alert runModal];
    if (res == NSAlertFirstButtonReturn) {
        completionHandler([input stringValue]);
    } else {
        completionHandler(nil);
    }
}

- (BOOL)windowShouldClose:(NSWindow *)sender {
    if (self.impl) {
        if (self.impl->force_closing) {
            return YES;
        }
        if (self.impl->close_cb) {
            int prevent = self.impl->close_cb(self.impl, self.impl->close_arg);
            if (prevent) {
                return NO; // PREVENT DEFAULT! Window stays open!
            }
        }
        return YES;
    }
    return YES;
}

- (void)windowWillClose:(NSNotification *)notification {
    if (self.impl && self.impl->is_main) {
        platform_kill_child_processes();
        exit(0);
    }
}
@end

@interface TauriAppDelegate : NSObject <NSApplicationDelegate>
+ (TauriAppDelegate *)sharedInstance;
- (void)terminateApp:(id)sender;
@end

@implementation TauriAppDelegate
+ (TauriAppDelegate *)sharedInstance {
    static TauriAppDelegate *inst = nil;
    if (!inst) {
        inst = [[TauriAppDelegate alloc] init];
    }
    return inst;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    return YES;
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {
    platform_kill_child_processes();
    return NSTerminateNow;
}

- (void)applicationWillTerminate:(NSNotification *)notification {
    platform_kill_child_processes();
}

- (void)terminateApp:(id)sender {
    platform_kill_child_processes();
    exit(0);
}
@end

static inline void run_on_main_sync(void (^block)(void)) {
    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_sync(dispatch_get_main_queue(), block);
    }
}

static inline void run_on_main_async(void (^block)(void)) {
    dispatch_async(dispatch_get_main_queue(), block);
}

static inline void platform_apply_app_name_macos(const char *name) {
    if (!name || name[0] == '\0') return;
    g_app_name = name;
    run_on_main_async(^{
        NSString *appName = [NSString stringWithUTF8String:g_app_name.c_str()];
        [[NSProcessInfo processInfo] setProcessName:appName];

        NSMenu *mainMenu = [NSApp mainMenu];
        if (mainMenu && [mainMenu numberOfItems] > 0) {
            NSMenuItem *appMenuItem = [mainMenu itemAtIndex:0];
            if (appMenuItem) {
                [appMenuItem setTitle:appName];
                NSMenu *appMenu = [appMenuItem submenu];
                if (appMenu) {
                    [appMenu setTitle:appName];
                    for (NSMenuItem *item in [appMenu itemArray]) {
                        SEL act = [item action];
                        if (act == @selector(orderFrontStandardAboutPanel:)) {
                            [item setTitle:[NSString stringWithFormat:@"About %@", appName]];
                        } else if (act == @selector(hide:)) {
                            [item setTitle:[NSString stringWithFormat:@"Hide %@", appName]];
                        } else if (act == @selector(terminate:)) {
                            [item setTitle:[NSString stringWithFormat:@"Quit %@", appName]];
                        }
                    }
                }
            }
        }
    });
}

static inline void setup_macos_default_menu(bool debug) {
    if ([NSApp mainMenu] != nil) return;

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

    // 1. Application Menu
    NSMenuItem *appMenuItem = [[NSMenuItem alloc] init];
    [appMenuItem setTitle:appName];
    [mainMenu addItem:appMenuItem];
    NSMenu *appMenu = [[NSMenu alloc] initWithTitle:appName];
    [appMenuItem setSubmenu:appMenu];

    NSString *aboutTitle = [NSString stringWithFormat:@"About %@", appName];
    [appMenu addItemWithTitle:aboutTitle action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *servicesItem = [[NSMenuItem alloc] initWithTitle:@"Services" action:nil keyEquivalent:@""];
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
    NSMenuItem *quitItem = [appMenu addItemWithTitle:quitTitle action:@selector(terminateApp:) keyEquivalent:@"q"];
    [quitItem setTarget:[TauriAppDelegate sharedInstance]];

    // 2. Edit Menu (Crucial for standard clipboard shortcuts in webview!)
    NSMenuItem *editMenuItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:editMenuItem];
    NSMenu *editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
    [editMenuItem setSubmenu:editMenu];

    [editMenu addItemWithTitle:@"Undo" action:@selector(undo:) keyEquivalent:@"z"];
    NSMenuItem *redoItem = [[NSMenuItem alloc] initWithTitle:@"Redo" action:@selector(redo:) keyEquivalent:@"Z"];
    [editMenu addItem:redoItem];
    [editMenu addItem:[NSMenuItem separatorItem]];
    [editMenu addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
    [editMenu addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
    [editMenu addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
    [editMenu addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];

    // 3. View Menu (Reload & Fullscreen)
    NSMenuItem *viewMenuItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:viewMenuItem];
    NSMenu *viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
    [viewMenuItem setSubmenu:viewMenu];

    if (debug) {
        [viewMenu addItemWithTitle:@"Reload" action:@selector(reload:) keyEquivalent:@"r"];
        [viewMenu addItem:[NSMenuItem separatorItem]];
    }
    NSMenuItem *fullscreenItem = [[NSMenuItem alloc] initWithTitle:@"Toggle Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
    [fullscreenItem setKeyEquivalentModifierMask:(NSEventModifierFlagControl | NSEventModifierFlagCommand)];
    [viewMenu addItem:fullscreenItem];

    // 4. Window Menu
    NSMenuItem *windowMenuItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:windowMenuItem];
    NSMenu *windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];
    [windowMenuItem setSubmenu:windowMenu];

    [windowMenu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [windowMenu addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
    [windowMenu addItem:[NSMenuItem separatorItem]];
    [windowMenu addItemWithTitle:@"Bring All to Front" action:@selector(arrangeInFront:) keyEquivalent:@""];
    [NSApp setWindowsMenu:windowMenu];

    [NSApp setMainMenu:mainMenu];
}

static inline webview_t webview_create(int debug, void *window) {
    __block WebviewImpl *impl = nullptr;
    run_on_main_sync(^{
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp setDelegate:[TauriAppDelegate sharedInstance]];
        setup_macos_default_menu(debug != 0);

        impl = new WebviewImpl();
        impl->is_main = !g_has_main_window;
        g_has_main_window = true;

        WKWebViewConfiguration *config = [[WKWebViewConfiguration alloc] init];
        if (debug) {
            [config.preferences setValue:@YES forKey:@"developerExtrasEnabled"];
        }

        impl->ucc = config.userContentController;
        TauriBridgeScriptHandler *handler = [[TauriBridgeScriptHandler alloc] init];
        handler.impl = impl;
        impl->handler = handler;
        [impl->ucc addScriptMessageHandler:handler name:@"__tauri_bridge__"];

        NSString *bridge_js =
            @"window.__TAURI_SEQ__ = 0;"
            @"window.__TAURI_CB__ = {};"
            @"window.__TAURI_BRIDGE_CALL__ = function(name, raw) {"
            @"  return new Promise(function(resolve, reject) {"
            @"    var seq = String(++window.__TAURI_SEQ__);"
            @"    window.__TAURI_CB__[seq] = { resolve: resolve, reject: reject };"
            @"    window.webkit.messageHandlers.__tauri_bridge__.postMessage({"
            @"      name: name,"
            @"      seq: seq,"
            @"      req: typeof raw === 'string' ? raw : JSON.stringify(raw)"
            @"    });"
            @"  });"
            @"};"
            @"window.__TAURI_BRIDGE_REPLY__ = function(seq, status, res) {"
            @"  var cb = window.__TAURI_CB__[seq];"
            @"  if (!cb) return;"
            @"  delete window.__TAURI_CB__[seq];"
            @"  if (status === 0) cb.resolve(res);"
            @"  else cb.reject(res);"
            @"};";

        WKUserScript *script = [[WKUserScript alloc] initWithSource:bridge_js
                                                      injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                                    forMainFrameOnly:YES];
        [impl->ucc addUserScript:script];

        NSRect frame = NSMakeRect(0, 0, 960, 640);
        NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                           NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
        impl->window = [[NSWindow alloc] initWithContentRect:frame
                                                   styleMask:style
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
        [impl->window setReleasedWhenClosed:NO];
        [impl->window setDelegate:handler];
        impl->wk = [[WKWebView alloc] initWithFrame:frame configuration:config];
        [impl->wk setNavigationDelegate:handler];
        [impl->wk setUIDelegate:handler];
        if (debug) {
            if ([impl->wk respondsToSelector:@selector(setInspectable:)]) {
                [impl->wk performSelector:@selector(setInspectable:) withObject:@YES];
            }
        }
        [impl->window setContentView:impl->wk];
    });
    return static_cast<webview_t>(impl);
}

static inline void webview_destroy(webview_t w) {
    if (!w) return;
    run_on_main_sync(^{
        WebviewImpl *impl = static_cast<WebviewImpl *>(w);
        [impl->window setDelegate:nil];
        [impl->window close];
        delete impl;
    });
}

static inline void webview_set_close_handler(webview_t w, webview_close_cb_fn fn, void *arg) {
    if (!w) return;
    WebviewImpl *impl = static_cast<WebviewImpl *>(w);
    impl->close_cb = fn;
    impl->close_arg = arg;
}

static inline void webview_force_close(webview_t w) {
    if (!w) return;
    run_on_main_sync(^{
        WebviewImpl *impl = static_cast<WebviewImpl *>(w);
        impl->force_closing = true;
        [impl->window setDelegate:nil];
        [impl->window close];
        if (impl->is_main) {
            exit(0);
        }
    });
}

static inline void webview_set_title(webview_t w, const char *title) {
    if (!w || !title) return;
    std::string s_title(title);
    run_on_main_sync(^{
        WebviewImpl *impl = static_cast<WebviewImpl *>(w);
        [impl->window setTitle:[NSString stringWithUTF8String:s_title.c_str()]];
    });
}

static inline void webview_set_size(webview_t w, int width, int height, int hints) {
    if (!w) return;
    run_on_main_sync(^{
        WebviewImpl *impl = static_cast<WebviewImpl *>(w);
        NSRect frame = impl->window.frame;
        frame.size = NSMakeSize(width, height);
        [impl->window setFrame:frame display:YES];
        [impl->window center];
        if (hints == 3) {
            NSUInteger mask = [impl->window styleMask] & ~NSWindowStyleMaskResizable;
            [impl->window setStyleMask:mask];
        }
    });
}

static inline void *webview_get_window(webview_t w) {
    if (!w) return nullptr;
    WebviewImpl *impl = static_cast<WebviewImpl *>(w);
    return (__bridge void *)impl->window;
}

static inline void *webview_get_native_handle(webview_t w, int kind) {
    return webview_get_window(w);
}

static inline void webview_init(webview_t w, const char *js) {
    if (!w || !js) return;
    std::string s_js(js);
    run_on_main_sync(^{
        WebviewImpl *impl = static_cast<WebviewImpl *>(w);
        NSString *src = [NSString stringWithUTF8String:s_js.c_str()];
        WKUserScript *script = [[WKUserScript alloc] initWithSource:src
                                                      injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                                   forMainFrameOnly:YES];
        [impl->ucc addUserScript:script];
    });
}

static inline void webview_eval(webview_t w, const char *js) {
    if (!w || !js) return;
    std::string s_js(js);
    run_on_main_async(^{
        WebviewImpl *impl = static_cast<WebviewImpl *>(w);
        NSString *src = [NSString stringWithUTF8String:s_js.c_str()];
        [impl->wk evaluateJavaScript:src completionHandler:nil];
    });
}

static inline void webview_bind(webview_t w, const char *name, webview_bind_fn fn, void *arg) {
    if (!w || !name) return;
    std::string s_name(name);
    run_on_main_sync(^{
        WebviewImpl *impl = static_cast<WebviewImpl *>(w);
        impl->bindings[s_name] = WebviewBinding{fn, arg};
    });
    std::string js = "window[\"" + s_name + "\"] = function(raw) { return window.__TAURI_BRIDGE_CALL__(\"" + s_name + "\", raw); };";
    webview_init(w, js.c_str());
    webview_eval(w, js.c_str());
}

static inline void webview_return(webview_t w, const char *seq, int status, const char *result) {
    if (!w || !seq) return;
    std::string s_seq = webview_js_escape(seq);
    std::string s_res = webview_js_escape(result ? result : "");
    std::string js = "window.__TAURI_BRIDGE_REPLY__(\"" + s_seq + "\", " + std::to_string(status) + ", \"" + s_res + "\");";
    webview_eval(w, js.c_str());
}

static inline void webview_dispatch(webview_t w, webview_dispatch_fn fn, void *arg) {
    run_on_main_async(^{
        fn(w, arg);
    });
}

static inline void webview_navigate(webview_t w, const char *url) {
    if (!w || !url) return;
    std::string s_url(url);
    run_on_main_sync(^{
        WebviewImpl *impl = static_cast<WebviewImpl *>(w);
        NSURL *nsurl = [NSURL URLWithString:[NSString stringWithUTF8String:s_url.c_str()]];
        if (nsurl) {
            NSURLRequest *req = [NSURLRequest requestWithURL:nsurl];
            [impl->wk loadRequest:req];
        }
    });
}

static inline void webview_run(webview_t w) {
    [NSApp run];
}

static inline void webview_terminate(webview_t w) {
    run_on_main_async(^{
        [NSApp stop:nil];
        NSEvent *ev = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                         location:NSMakePoint(0, 0)
                                    modifierFlags:0
                                        timestamp:0
                                     windowNumber:0
                                          context:nil
                                          subtype:0
                                            data1:0
                                            data2:0];
        [NSApp postEvent:ev atStart:YES];
    });
}
#elif defined(_WIN32)
#include <windows.h>
#include <wrl.h>
#include <WebView2.h>
#include <map>
#include <string>

using namespace Microsoft::WRL;

struct WebviewImpl {
    HWND hwnd;
    ICoreWebView2Controller *controller;
    ICoreWebView2 *webview;
    bool is_main;
    std::map<std::string, WebviewBinding> bindings;
    webview_close_cb_fn close_cb = nullptr;
    void *close_arg = nullptr;
    bool force_closing = false;
};

static bool g_has_main_window = false;

static LRESULT CALLBACK WebviewWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WebviewImpl *impl = reinterpret_cast<WebviewImpl *>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_SIZE: {
            if (impl && impl->controller) {
                RECT bounds;
                GetClientRect(hwnd, &bounds);
                impl->controller->put_Bounds(bounds);
            }
            return 0;
        }
        case WM_CLOSE: {
            if (impl) {
                if (!impl->force_closing && impl->close_cb) {
                    int prevent = impl->close_cb(impl, impl->close_arg);
                    if (prevent) {
                        return 0; // PREVENT DEFAULT! Window stays open!
                    }
                }
                if (impl->is_main) {
                    PostQuitMessage(0);
                } else {
                    DestroyWindow(hwnd);
                }
            }
            return 0;
        }
        case WM_USER + 101: {
            struct WinDispatchArg {
                webview_t w;
                webview_dispatch_fn fn;
                void *arg;
            };
            WinDispatchArg *p = reinterpret_cast<WinDispatchArg *>(lp);
            if (p) {
                p->fn(p->w, p->arg);
                delete p;
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static inline webview_t webview_create(int debug, void *window) {
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = WebviewWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TauriLuaWebviewClass";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0, L"TauriLuaWebviewClass", L"Tauri App",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (!hwnd) return nullptr;

    WebviewImpl *impl = new WebviewImpl();
    impl->hwnd = hwnd;
    impl->controller = nullptr;
    impl->webview = nullptr;
    impl->is_main = !g_has_main_window;
    g_has_main_window = true;

    SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl));

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, nullptr, nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [impl, hwnd, debug](HRESULT result, ICoreWebView2Environment *env) -> HRESULT {
                if (FAILED(result) || !env) return result;
                env->CreateCoreWebView2Controller(
                    hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [impl, hwnd, debug](HRESULT res, ICoreWebView2Controller *ctrl) -> HRESULT {
                            if (FAILED(res) || !ctrl) return res;
                            impl->controller = ctrl;
                            ctrl->AddRef();

                            ctrl->get_CoreWebView2(&impl->webview);
                            if (impl->webview) {
                                ICoreWebView2Settings *settings = nullptr;
                                impl->webview->get_Settings(&settings);
                                if (settings) {
                                    settings->put_AreDevToolsEnabled(debug ? TRUE : FALSE);
                                    settings->put_IsScriptEnabled(TRUE);
                                    settings->Release();
                                }

                                RECT bounds;
                                GetClientRect(hwnd, &bounds);
                                ctrl->put_Bounds(bounds);
                                ctrl->put_IsVisible(TRUE);

                                EventRegistrationToken token;
                                impl->webview->add_WebMessageReceived(
                                    Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                        [impl](ICoreWebView2 *sender, ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
                                            LPWSTR json_str = nullptr;
                                            args->get_WebMessageAsJson(&json_str);
                                            if (json_str) {
                                                LPWSTR source_w = nullptr;
                                                if (SUCCEEDED(args->get_Source(&source_w)) && source_w) {
                                                    int slen = WideCharToMultiByte(CP_UTF8, 0, source_w, -1, NULL, 0, NULL, NULL);
                                                    std::string source_u8(slen > 0 ? slen : 0, '\0');
                                                    if (slen > 0) {
                                                        int swritten = WideCharToMultiByte(CP_UTF8, 0, source_w, -1, &source_u8[0], slen, NULL, NULL);
                                                        if (swritten > 0) source_u8.resize(static_cast<size_t>(swritten - 1));
                                                    }
                                                    CoTaskMemFree(source_w);
                                                    LPWSTR top_w = nullptr;
                                                    std::string top_u8;
                                                    if (SUCCEEDED(sender->get_Source(&top_w)) && top_w) {
                                                        int tlen = WideCharToMultiByte(CP_UTF8, 0, top_w, -1, NULL, 0, NULL, NULL);
                                                        if (tlen > 0) {
                                                            top_u8.resize(tlen);
                                                            int twritten = WideCharToMultiByte(CP_UTF8, 0, top_w, -1, &top_u8[0], tlen, NULL, NULL);
                                                            if (twritten > 0) top_u8.resize(static_cast<size_t>(twritten - 1));
                                                        }
                                                        CoTaskMemFree(top_w);
                                                    }
                                                    if (!SecurityPolicy::is_frame_source_allowed(source_u8, top_u8, g_dev_mode != 0, g_dev_url)) {
                                                        CoTaskMemFree(json_str);
                                                        return S_OK; // Reject cross-origin or untrusted subframe message
                                                    }
                                                }

                                                int len = WideCharToMultiByte(CP_UTF8, 0, json_str, -1, NULL, 0, NULL, NULL);
                                                std::string u8(len > 0 ? len : 0, '\0');
                                                if (len > 0) {
                                                    int written = WideCharToMultiByte(CP_UTF8, 0, json_str, -1, &u8[0], len, NULL, NULL);
                                                    if (written > 0) {
                                                        u8.resize(static_cast<size_t>(written - 1));
                                                    }
                                                }
                                                CoTaskMemFree(json_str);

                                                std::string name = extract_json_string(u8, "name");
                                                std::string seq = extract_json_string(u8, "seq");
                                                std::string req = extract_json_string(u8, "req");

                                                auto it = impl->bindings.find(name);
                                                if (it != impl->bindings.end() && it->second.fn) {
                                                    it->second.fn(seq.c_str(), req.c_str(), it->second.arg);
                                                }
                                            }
                                            return S_OK;
                                        }).Get(),
                                    &token);

                                EventRegistrationToken navToken;
                                impl->webview->add_NavigationStarting(
                                    Callback<ICoreWebView2NavigationStartingEventHandler>(
                                        [](ICoreWebView2 *sender, ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
                                            (void)sender;
                                            LPWSTR uri_w = nullptr;
                                            if (SUCCEEDED(args->get_Uri(&uri_w)) && uri_w) {
                                                int len = WideCharToMultiByte(CP_UTF8, 0, uri_w, -1, NULL, 0, NULL, NULL);
                                                std::string u8(len > 0 ? len : 0, '\0');
                                                if (len > 0) {
                                                    int written = WideCharToMultiByte(CP_UTF8, 0, uri_w, -1, &u8[0], len, NULL, NULL);
                                                    if (written > 0) u8.resize(static_cast<size_t>(written - 1));
                                                }
                                                CoTaskMemFree(uri_w);
                                                if (!SecurityPolicy::is_navigation_allowed(u8, g_dev_mode != 0, g_dev_url)) {
                                                    args->put_Cancel(TRUE);
                                                }
                                            }
                                            return S_OK;
                                        }).Get(),
                                    &navToken);

                                EventRegistrationToken newWinToken;
                                impl->webview->add_NewWindowRequested(
                                    Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                        [](ICoreWebView2 *sender, ICoreWebView2NewWindowRequestedEventArgs *args) -> HRESULT {
                                            (void)sender;
                                            args->put_Handled(TRUE);
                                            return S_OK;
                                        }).Get(),
                                    &newWinToken);
                            }
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());

    MSG msg;
    while (!impl->controller && GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    const char *bridge_js =
        "window.__TAURI_SEQ__ = 0;"
        "window.__TAURI_CB__ = {};"
        "window.__TAURI_BRIDGE_CALL__ = function(name, raw) {"
        "  return new Promise(function(resolve, reject) {"
        "    var seq = String(++window.__TAURI_SEQ__);"
        "    window.__TAURI_CB__[seq] = { resolve: resolve, reject: reject };"
        "    window.chrome.webview.postMessage({"
        "      name: name,"
        "      seq: seq,"
        "      req: typeof raw === 'string' ? raw : JSON.stringify(raw)"
        "    });"
        "  });"
        "};"
        "window.__TAURI_BRIDGE_REPLY__ = function(seq, status, res) {"
        "  var cb = window.__TAURI_CB__[seq];"
        "  if (!cb) return;"
        "  delete window.__TAURI_CB__[seq];"
        "  if (status === 0) { cb.resolve(res); } else { cb.reject(res); }"
        "};";

    webview_init(impl, bridge_js);
    return static_cast<webview_t>(impl);
}

static inline void webview_destroy(webview_t w) {
    if (!w) return;
    WebviewImpl *impl = static_cast<WebviewImpl *>(w);
    if (impl->controller) {
        impl->controller->Close();
        impl->controller->Release();
    }
    if (impl->webview) {
        impl->webview->Release();
    }
    DestroyWindow(impl->hwnd);
    delete impl;
}

static inline void webview_set_close_handler(webview_t w, webview_close_cb_fn fn, void *arg) {
    if (!w) return;
    WebviewImpl *impl = static_cast<WebviewImpl *>(w);
    impl->close_cb = fn;
    impl->close_arg = arg;
}

static inline void webview_force_close(webview_t w) {
    if (!w) return;
    WebviewImpl *impl = static_cast<WebviewImpl *>(w);
    impl->force_closing = true;
    PostMessage(impl->hwnd, WM_CLOSE, 0, 0);
}

static inline void webview_set_title(webview_t w, const char *title) {
    WebviewImpl *impl = static_cast<WebviewImpl *>(w);
    int len = MultiByteToWideChar(CP_UTF8, 0, title, -1, NULL, 0);
    std::wstring wtitle(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, title, -1, &wtitle[0], len);
    SetWindowTextW(impl->hwnd, wtitle.c_str());
}

static inline void webview_set_size(webview_t w, int width, int height, int hints) {
    WebviewImpl *impl = static_cast<WebviewImpl *>(w);
    SetWindowPos(impl->hwnd, NULL, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER);
}

static inline void *webview_get_window(webview_t w) {
    WebviewImpl *impl = static_cast<WebviewImpl *>(w);
    return impl->hwnd;
}

static inline void *webview_get_native_handle(webview_t w, int kind) {
    WebviewImpl *impl = static_cast<WebviewImpl *>(w);
    if (kind == WEBVIEW_NATIVE_HANDLE_KIND_UI_WINDOW) return impl->hwnd;
    if (kind == WEBVIEW_NATIVE_HANDLE_KIND_BROWSER_CONTROLLER) return impl->controller;
    return nullptr;
}

static inline void webview_init(webview_t w, const char *js) {
    WebviewImpl *impl = static_cast<WebviewImpl *>(w);
    if (!impl->webview) return;
    int len = MultiByteToWideChar(CP_UTF8, 0, js, -1, NULL, 0);
    std::wstring wjs(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, js, -1, &wjs[0], len);
    impl->webview->AddScriptToExecuteOnDocumentCreated(wjs.c_str(), NULL);
}

static inline void webview_eval(webview_t w, const char *js) {
    WebviewImpl *impl = static_cast<WebviewImpl *>(w);
    if (!impl->webview) return;
    int len = MultiByteToWideChar(CP_UTF8, 0, js, -1, NULL, 0);
    std::wstring wjs(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, js, -1, &wjs[0], len);
    impl->webview->ExecuteScript(wjs.c_str(), NULL);
}

static inline void webview_bind(webview_t w, const char *name, webview_bind_fn fn, void *arg) {
    WebviewImpl *impl = static_cast<WebviewImpl *>(w);
    impl->bindings[name] = WebviewBinding{fn, arg};
    std::string js =
        "window['" + std::string(name) + "'] = function(raw) {"
        "  return window.__TAURI_BRIDGE_CALL__('" + std::string(name) + "', raw);"
        "};";
    webview_init(w, js.c_str());
    webview_eval(w, js.c_str());
}

static inline void webview_return(webview_t w, const char *seq, int status, const char *result) {
    if (!w || !seq) return;
    std::string s_seq = webview_js_escape(seq);
    std::string s_res = webview_js_escape(result ? result : "");
    std::string js = "window.__TAURI_BRIDGE_REPLY__(\"" + s_seq + "\", " + std::to_string(status) + ", \"" + s_res + "\");";
    webview_eval(w, js.c_str());
}

static inline void webview_dispatch(webview_t w, webview_dispatch_fn fn, void *arg) {
    WebviewImpl *impl = static_cast<WebviewImpl *>(w);
    struct WinDispatchArg {
        webview_t w;
        webview_dispatch_fn fn;
        void *arg;
    };
    WinDispatchArg *p = new WinDispatchArg{w, fn, arg};
    PostMessageW(impl->hwnd, WM_USER + 101, 0, reinterpret_cast<LPARAM>(p));
}

static inline void webview_navigate(webview_t w, const char *url) {
    WebviewImpl *impl = static_cast<WebviewImpl *>(w);
    if (!impl->webview) return;
    int len = MultiByteToWideChar(CP_UTF8, 0, url, -1, NULL, 0);
    std::wstring wurl(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, url, -1, &wurl[0], len);
    impl->webview->Navigate(wurl.c_str());
}

static inline void webview_run(webview_t w) {
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

static inline void webview_terminate(webview_t w) {
    PostQuitMessage(0);
}

#else
// ==========================================
// Linux (GTK3 + WebKit2GTK)
// ==========================================
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <map>
#include <string>

struct WebviewImpl {
    GtkWidget *window;
    GtkWidget *wk;
    WebKitUserContentManager *ucc;
    bool is_main;
    std::map<std::string, WebviewBinding> bindings;
    webview_close_cb_fn close_cb = nullptr;
    void *close_arg = nullptr;
    bool force_closing = false;
};

static bool g_has_main_window = false;

static void on_gtk_script_message(WebKitUserContentManager *ucc, WebKitJavascriptResult *res, gpointer user_data) {
    WebviewImpl *impl = static_cast<WebviewImpl *>(user_data);
    if (!impl) return;

    JSCValue *val = webkit_javascript_result_get_js_value(res);
    if (jsc_value_is_object(val)) {
        JSCValue *name_v = jsc_value_object_get_property(val, "name");
        JSCValue *seq_v = jsc_value_object_get_property(val, "seq");
        JSCValue *req_v = jsc_value_object_get_property(val, "req");

        char *name = jsc_value_to_string(name_v);
        char *seq = jsc_value_to_string(seq_v);
        char *req = jsc_value_to_string(req_v);

        if (name && seq && req) {
            auto it = impl->bindings.find(name);
            if (it != impl->bindings.end() && it->second.fn) {
                it->second.fn(seq, req, it->second.arg);
            }
        }
        g_free(name); g_free(seq); g_free(req);
        g_object_unref(name_v); g_object_unref(seq_v); g_object_unref(req_v);
    }
}

static void on_gtk_window_destroy(GtkWidget *widget, gpointer data) {
    (void)widget; (void)data;
    platform_kill_child_processes();
    gtk_main_quit();
}

static gboolean on_gtk_window_delete_event(GtkWidget *widget, GdkEvent *event, gpointer data) {
    WebviewImpl *impl = static_cast<WebviewImpl *>(data);
    if (impl) {
        if (!impl->force_closing && impl->close_cb) {
            int prevent = impl->close_cb(impl, impl->close_arg);
            if (prevent) {
                return TRUE; // PREVENT DEFAULT in GTK!
            }
        }
        platform_kill_child_processes();
        gtk_main_quit();
    }
    return FALSE;
}

static inline webview_t webview_create(int debug, void *window) {
#if defined(__linux__) || (!defined(_WIN32) && !defined(__APPLE__))
    setenv("NO_AT_BRIDGE", "1", 0);
    setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", 0);
#endif
    if (!gtk_init_check(NULL, NULL)) {
        return nullptr;
    }

    WebviewImpl *impl = new WebviewImpl();
    impl->is_main = !g_has_main_window;
    g_has_main_window = true;

    impl->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(impl->window), 800, 600);

    WebKitWebContext *context = webkit_web_context_get_default();
    impl->ucc = webkit_user_content_manager_new();

    WebKitSettings *settings = webkit_settings_new();
    if (debug) {
        webkit_settings_set_enable_developer_extras(settings, TRUE);
    }
    webkit_settings_set_enable_javascript(settings, TRUE);

    impl->wk = webkit_web_view_new_with_user_content_manager(impl->ucc);
    webkit_web_view_set_settings(WEBKIT_WEB_VIEW(impl->wk), settings);

    webkit_user_content_manager_register_script_message_handler(impl->ucc, "__tauri_bridge__");
    g_signal_connect(impl->ucc, "script-message-received::__tauri_bridge__", G_CALLBACK(on_gtk_script_message), impl);
    g_signal_connect(impl->window, "delete-event", G_CALLBACK(on_gtk_window_delete_event), impl);
    g_signal_connect(impl->window, "destroy", G_CALLBACK(on_gtk_window_destroy), impl);

    g_signal_connect(impl->wk, "decide-policy", G_CALLBACK(+[](WebKitWebView *web_view, WebKitPolicyDecision *decision, WebKitPolicyDecisionType type, gpointer user_data) -> gboolean {
        (void)web_view; (void)user_data;
        if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
            WebKitNavigationPolicyDecision *nav_decision = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
            WebKitNavigationAction *action = webkit_navigation_policy_decision_get_navigation_action(nav_decision);
            WebKitURIRequest *req = webkit_navigation_action_get_request(action);
            const gchar *uri = webkit_uri_request_get_uri(req);
            if (uri && !SecurityPolicy::is_navigation_allowed(uri, g_dev_mode != 0, g_dev_url)) {
                webkit_policy_decision_ignore(decision);
                return TRUE;
            }
        }
        return FALSE;
    }), NULL);

    g_signal_connect(impl->wk, "create", G_CALLBACK(+[](WebKitWebView *web_view, WebKitNavigationAction *nav_action, gpointer user_data) -> GtkWidget * {
        (void)web_view; (void)nav_action; (void)user_data;
        return NULL;
    }), NULL);

    gtk_container_add(GTK_CONTAINER(impl->window), impl->wk);
    gtk_widget_show_all(impl->window);

    const char *bridge_js =
        "window.__TAURI_SEQ__ = 0;"
        "window.__TAURI_CB__ = {};"
        "window.__TAURI_BRIDGE_CALL__ = function(name, raw) {"
        "  return new Promise(function(resolve, reject) {"
        "    var seq = String(++window.__TAURI_SEQ__);"
        "    window.__TAURI_CB__[seq] = { resolve: resolve, reject: reject };"
        "    window.webkit.messageHandlers.__tauri_bridge__.postMessage({"
        "      name: name,"
        "      seq: seq,"
        "      req: typeof raw === 'string' ? raw : JSON.stringify(raw)"
        "    });"
        "  });"
        "};"
        "window.__TAURI_BRIDGE_REPLY__ = function(seq, status, res) {"
        "  var cb = window.__TAURI_CB__[seq];"
        "  if (!cb) return;"
        "  delete window.__TAURI_CB__[seq];"
        "  if (status === 0) { cb.resolve(res); } else { cb.reject(res); }"
        "};";

    WebKitUserScript *script = webkit_user_script_new(
        bridge_js, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
    webkit_user_content_manager_add_script(impl->ucc, script);
    webkit_user_script_unref(script);

    return static_cast<webview_t>(impl);
}

static inline void webview_destroy(webview_t w) {
    if (!w) return;
    WebviewImpl *impl = static_cast<WebviewImpl *>(w);
    gtk_widget_destroy(impl->window);
    delete impl;
}

static inline void webview_set_close_handler(webview_t w, webview_close_cb_fn fn, void *arg) {
    if (!w) return;
    WebviewImpl *impl = static_cast<WebviewImpl *>(w);
    impl->close_cb = fn;
    impl->close_arg = arg;
}

static inline void webview_force_close(webview_t w) {
    if (!w) return;
    WebviewImpl *impl = static_cast<WebviewImpl *>(w);
    impl->force_closing = true;
    if (impl->is_main) {
        gtk_main_quit();
    } else {
        gtk_widget_destroy(impl->window);
    }
}

static inline void webview_set_title(webview_t w, const char *title) {
    WebviewImpl *impl = static_cast<WebviewImpl *>(w);
    gtk_window_set_title(GTK_WINDOW(impl->window), title);
}

static inline void webview_set_size(webview_t w, int width, int height, int hints) {
    WebviewImpl *impl = static_cast<WebviewImpl *>(w);
    gtk_window_set_default_size(GTK_WINDOW(impl->window), width, height);
    gtk_window_resize(GTK_WINDOW(impl->window), width, height);
    if (hints == 3) {
        gtk_window_set_resizable(GTK_WINDOW(impl->window), FALSE);
    } else {
        gtk_window_set_resizable(GTK_WINDOW(impl->window), TRUE);
    }
}

static inline void *webview_get_window(webview_t w) {
    WebviewImpl *impl = static_cast<WebviewImpl *>(w);
    return impl->window;
}

static inline void *webview_get_native_handle(webview_t w, int kind) {
    WebviewImpl *impl = static_cast<WebviewImpl *>(w);
    if (kind == WEBVIEW_NATIVE_HANDLE_KIND_UI_WINDOW) return impl->window;
    if (kind == WEBVIEW_NATIVE_HANDLE_KIND_UI_WIDGET) return impl->wk;
    return nullptr;
}

static inline void webview_init(webview_t w, const char *js) {
    WebviewImpl *impl = static_cast<WebviewImpl *>(w);
    WebKitUserScript *script = webkit_user_script_new(
        js, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
    webkit_user_content_manager_add_script(impl->ucc, script);
    webkit_user_script_unref(script);
}

static inline void webview_eval(webview_t w, const char *js) {
    WebviewImpl *impl = static_cast<WebviewImpl *>(w);
    webkit_web_view_run_javascript(WEBKIT_WEB_VIEW(impl->wk), js, NULL, NULL, NULL);
}

static inline void webview_bind(webview_t w, const char *name, webview_bind_fn fn, void *arg) {
    WebviewImpl *impl = static_cast<WebviewImpl *>(w);
    impl->bindings[name] = WebviewBinding{fn, arg};
    std::string js =
        "window['" + std::string(name) + "'] = function(raw) {"
        "  return window.__TAURI_BRIDGE_CALL__('" + std::string(name) + "', raw);"
        "};";
    webview_init(w, js.c_str());
    webview_eval(w, js.c_str());
}

static inline void webview_return(webview_t w, const char *seq, int status, const char *result) {
    if (!w || !seq) return;
    std::string s_seq = webview_js_escape(seq);
    std::string s_res = webview_js_escape(result ? result : "");
    std::string js = "window.__TAURI_BRIDGE_REPLY__(\"" + s_seq + "\", " + std::to_string(status) + ", \"" + s_res + "\");";
    webview_eval(w, js.c_str());
}

struct GtkDispatchArg {
    webview_t w;
    webview_dispatch_fn fn;
    void *arg;
};

static gboolean on_gtk_idle_dispatch(gpointer data) {
    GtkDispatchArg *p = static_cast<GtkDispatchArg *>(data);
    p->fn(p->w, p->arg);
    delete p;
    return G_SOURCE_REMOVE;
}

static inline void webview_dispatch(webview_t w, webview_dispatch_fn fn, void *arg) {
    GtkDispatchArg *p = new GtkDispatchArg{w, fn, arg};
    g_idle_add(on_gtk_idle_dispatch, p);
}

static inline void webview_navigate(webview_t w, const char *url) {
    WebviewImpl *impl = static_cast<WebviewImpl *>(w);
    webkit_web_view_load_uri(WEBKIT_WEB_VIEW(impl->wk), url);
}

static inline void webview_run(webview_t w) {
    gtk_main();
}

static inline void webview_terminate(webview_t w) {
    gtk_main_quit();
}
#endif

#endif