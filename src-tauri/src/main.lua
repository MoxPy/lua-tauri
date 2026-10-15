local Tauri = require("tauri")

local conf = {}
if __EMBEDDED_CONFIG_JSON__ then
    conf = Tauri.json.decode(__EMBEDDED_CONFIG_JSON__) or {}
end

local app = Tauri.init(conf)

if not app.state:has("visit_count") then
    app.state:set("visit_count", 0)
    app.state:set("session_meta", {
        app_name = "TauriLua State Demo",
        started_at = os.time(),
        nodes = {}
    })
end

app:register("increment_counter", function()
    local current = app.state:get("visit_count") or 0
    local next_val = current + 1
    app.state:set("visit_count", next_val)

    local meta = app.state:get("session_meta") or {}
    meta.last_worker_id = __ISOLATE_WORKER_ID__ or 0
    app.state:set("session_meta", meta)

    return {
        count = next_val,
        workerId = __ISOLATE_WORKER_ID__ or 0
    }
end)

app:register("get_shared_data", function()
    return {
        count = app.state:get("visit_count"),
        meta = app.state:get("session_meta"),
        workerId = __ISOLATE_WORKER_ID__ or 0
    }
end)

app:register("parallel_workload", function(payload)
    local isolate_id = __ISOLATE_WORKER_ID__ or 0
    local limit = tonumber(payload.limit) or 1500000
    local count = 0
    local start_time = os.clock()

    for i = 2, limit do
        local is_prime = true
        local max_d = math.floor(math.sqrt(i))
        for d = 2, max_d do
            if i % d == 0 then
                is_prime = false
                break
            end
        end
        if is_prime then
            count = count + 1
        end
    end

    local current = app.state:get("visit_count") or 0
    app.state:set("visit_count", current + 1)

    return {
        isolateId = isolate_id,
        limit = limit,
        primeCount = count,
        totalCalls = current + 1,
        elapsedSeconds = os.clock() - start_time
    }
end)

app:register("stream_countdown", function(payload, caller, channel)
    local count = tonumber(payload and payload.count) or 5
    local ch = channel or (payload and payload.onEvent)

    if ch then
        for i = count, 1, -1 do
            ch:send({
                remaining = i,
                message = string.format("Streaming countdown tick #%d", i),
                timestamp = os.time()
            })
            Tauri.sleep(200)
        end
    end

    return {
        status = "completed",
        totalTicks = count
    }
end)

if os.getenv("TAURI_SECURITY_TEST") == "1" and not __IS_ISOLATE_WORKER__ then
    local res = app.commands["tauri:shell:execute"]({
        program = "echo",
        args = {"SAFE_PROCESS_TEST", "; touch should_not_exist.txt"}
    })
    local f = io.open("should_not_exist.txt", "r")
    if f then
        f:close()
        os.remove("should_not_exist.txt")
        error("[CYBERSECURITY FAILURE] Command injection detected!")
    end
    local res2 = app.commands["tauri:shell:execute"]({
        command = "echo \"Arg with spaces\" 'Single quote arg' final_arg"
    })
    if not (res2 and res2.code == 0 and res2.stdout:find("Arg with spaces Single quote arg final_arg")) then
        error("[CYBERSECURITY FAILURE] Tokenized command execution failed!")
    end
    app:set_csp("default-src 'self' tauri: asset:;")

    -- Window Lifecycle Test
    local lifecycle_tested = false
    app:on_window_event(function(event)
        if event.event == "close-requested" then
            lifecycle_tested = true
            event:prevent_default()
        end
    end)
    local prevent_val = __TAURI_DISPATCH_WINDOW_CLOSE__("main")
    if not (lifecycle_tested and prevent_val == 1) then
        error("[WINDOW LIFECYCLE FAILURE] prevent_default did not prevent close!")
    end

    -- Single-Instance Test
    local single_instance_received = false
    local test_args = nil
    app:on_single_instance(function(payload)
        single_instance_received = true
        test_args = payload.args
    end)
    __TAURI_DISPATCH_SINGLE_INSTANCE__(Tauri.json.encode({
        args = {"tauri-app", "--open", "document.lua"},
        cwd = "/tmp"
    }))
    if not (single_instance_received and test_args and test_args[2] == "--open") then
        error("[SINGLE INSTANCE FAILURE] Secondary instance payload not received!")
    end

    -- Notification API Test
    app:notify({ title = "Test Notification", body = "Tauri Lua Notification Suite" })
    Tauri.notification.send("Direct Notification", "Testing global notification helper")

    -- Theme Detection & Event Test
    local current_theme = app:get_theme()
    if not (current_theme == "dark" or current_theme == "light") then
        error("[THEME FAILURE] Unexpected theme value: " .. tostring(current_theme))
    end
    local theme_changed_received = false
    app:on_theme_changed(function(theme)
        theme_changed_received = true
    end)
    __TAURI_DISPATCH_THEME__("dark")
    if not theme_changed_received then
        error("[THEME FAILURE] Theme change event not dispatched!")
    end

    -- Native File Drop Test
    local file_drop_received = false
    local dropped_paths = nil
    app:on_file_drop(function(event)
        file_drop_received = true
        dropped_paths = event.paths
    end)
    __TAURI_DISPATCH_FILE_DROP__("main", "drop", Tauri.json.encode({
        paths = {"/Users/test/image.png", "/Users/test/notes.pdf"},
        position = { x = 150, y = 250 }
    }))
    if not (file_drop_received and dropped_paths and #dropped_paths == 2) then
        error("[FILE DROP FAILURE] File drop event not received!")
    end

    -- Advanced Clipboard Test
    app.clipboard:write_text("Hello Advanced Clipboard Text")
    local clip_text = app.clipboard:read_text()
    if clip_text ~= "Hello Advanced Clipboard Text" then
        error("[CLIPBOARD FAILURE] Plain text clipboard mismatch: " .. tostring(clip_text))
    end

    app.clipboard:write_html("<p>Rich <b>Text</b></p>")
    local clip_html = app.clipboard:read_html()
    if clip_html and clip_html ~= "" then
        print("[CLIPBOARD] Read HTML successfully:", clip_html)
    end

    -- Write 1x1 PNG base64 to clipboard
    local test_png_b64 = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="
    app.clipboard:write_image(test_png_b64)
    local read_img = app.clipboard:read_image()
    if read_img and read_img ~= "" then
        print("[CLIPBOARD] Read Image successfully, bytes length:", #read_img)
    end

    app.clipboard:clear()

    -- Plugin Architecture Test
    local plugin_setup_called = false
    local plugin_theme_received = false
    local demo_plugin = Tauri.plugin("test-plugin", {
        setup = function(app_inst, self_plugin)
            plugin_setup_called = true
            app_inst:register("plugin:test-plugin|echo", function(args)
                return "plugin_echo: " .. tostring(args and args.text or "")
            end)
        end,
        on_theme_changed = function(app_inst, theme)
            plugin_theme_received = true
        end
    })

    app:plugin(demo_plugin)

    if not plugin_setup_called then
        error("[PLUGIN FAILURE] Plugin setup hook was not invoked!")
    end
    if not (app.plugins["test-plugin"] and app.commands["plugin:test-plugin|echo"]) then
        error("[PLUGIN FAILURE] Plugin or command not registered!")
    end
    local plugin_res = app.commands["plugin:test-plugin|echo"]({ text = "Antigravity" })
    if plugin_res ~= "plugin_echo: Antigravity" then
        error("[PLUGIN FAILURE] Plugin command result mismatch: " .. tostring(plugin_res))
    end
    __TAURI_DISPATCH_THEME__("light")
    if not plugin_theme_received then
        error("[PLUGIN FAILURE] Plugin theme event hook was not invoked!")
    end
    print("[PLUGIN ARCHITECTURE TEST PASSED]")

    print("[CYBERSECURITY, LIFECYCLE, PLUGINS, NOTIFICATIONS, SINGLE-INSTANCE, THEME, FILE-DROP & CLIPBOARD ALL TESTS PASSED]")
    os.exit(0)
end

if not __IS_ISOLATE_WORKER__ then
    app:on_window_event(function(event)
        if event.event == "close-requested" then
            print("[LIFECYCLE] Window close requested for:", event.label)
        end
    end)

    app:on_single_instance(function(data)
        print("[SINGLE-INSTANCE] Second instance launched with args:", Tauri.json.encode(data.args or {}))
        app:notify("Tauri Lua App", "Focus switched from second instance attempt.")
    end)
    local Menu = Tauri.Menu
    local Submenu = Tauri.Submenu
    local MenuItem = Tauri.MenuItem
    local CheckMenuItem = Tauri.CheckMenuItem
    local PredefinedMenuItem = Tauri.PredefinedMenuItem

    local menu = Menu.new({
        items = {
            Submenu.new({
                text = "File",
                items = {
                    MenuItem.new({ id = "new_project", text = "New Project", accelerator = "CmdOrCtrl+N" }),
                    MenuItem.new({ id = "open_file", text = "Open...", accelerator = "CmdOrCtrl+O" }),
                    PredefinedMenuItem.separator(),
                    PredefinedMenuItem.close_window(),
                }
            }),
            Submenu.new({
                text = "Edit",
                items = {
                    PredefinedMenuItem.undo(),
                    PredefinedMenuItem.redo(),
                    PredefinedMenuItem.separator(),
                    PredefinedMenuItem.cut(),
                    PredefinedMenuItem.copy(),
                    PredefinedMenuItem.paste(),
                    PredefinedMenuItem.select_all(),
                }
            }),
            Submenu.new({
                text = "View",
                items = {
                    CheckMenuItem.new({ id = "toggle_fps", text = "Show Performance Metrics", checked = true }),
                    PredefinedMenuItem.separator(),
                    PredefinedMenuItem.fullscreen(),
                }
            }),
            Submenu.new({
                text = "Window",
                items = {
                    PredefinedMenuItem.minimize(),
                    PredefinedMenuItem.zoom(),
                    PredefinedMenuItem.separator(),
                    PredefinedMenuItem.close_window(),
                }
            }),
            Submenu.new({
                text = "Help",
                items = {
                    MenuItem.new({ id = "docs", text = "Documentation" }),
                    PredefinedMenuItem.about(),
                }
            })
        }
    })
    app:set_menu(menu)

    app:on_menu(function(event)
        print("[NATIVE MENU CLICKED]", event.id)
    end)
end

app:create_main_window()