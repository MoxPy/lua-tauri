# Tauri-Lua ⚡️

> **Experimental desktop runtime combining an OS system webview with a lightweight C++17 native bridge and LuaJIT.**

[![Platform](https://img.shields.io/badge/Platforms-macOS%20%7C%20Linux%20%7C%20Windows%20(Experimental)-blue.svg)](#platform--capability-support-matrix)
[![Engine](https://img.shields.io/badge/Engine-LuaJIT%202.1%20%2B%20C%2B%2B17-orange.svg)](#architecture-overview)
[![LuaRocks](https://img.shields.io/badge/LuaRocks-Hermetic%20Ecosystem-blueviolet.svg)](#hermetic-luarocks-integration--dependency-ecosystem)
[![Host Embedding](https://img.shields.io/badge/Host%20Embedding-libtaurilua%20(C%20ABI)-blue.svg)](#scripting-engine-host--cc-embedding-libtaurilua)
[![Plugins](https://img.shields.io/badge/Plugins-lua--tauri--plugins-success.svg)](#plugin-system--extensibility)
[![License](https://img.shields.io/badge/License-MIT-purple.svg)](#license)

> [!NOTE]
> **Experimental Project Notice**<br>
> This project is an independent engineering exploration demonstrating how a webview-based desktop application architecture, security controls, and developer workflows can be implemented using a minimal C++17 native bridge and LuaJIT. It is research-oriented and not affiliated with or endorsed by The Tauri Programme.

---

## Executive Summary

**Tauri-Lua** combines modern web frontends with a desktop backend written in Lua, running on **LuaJIT** with a C++17 system bridge.

It explores Tauri-inspired patterns—including capabilities-based security, IPC envelopes, native packaging, and LuaRocks dependency isolation—in an interpreted, scripting-driven environment.

### Core Architecture Highlights:
* **Embeddable Scripting Host (`libtaurilua`):** Integrates into host C/C++ applications via a clean C ABI (`include/taurilua.h`).
* **Hermetic LuaRocks Package Ecosystem:** Installs and isolates Lua modules into local project trees (`src-tauri/.rocks/`), packaging them into release bundles.
* **Defense-in-Depth Security Model:** Direct process spawning (bypassing raw subshells for frontend commands), Content Security Policy (CSP) enforcement, strict Native Origin Gating, and declarative capability & scope access controls.
* **Non-Blocking Multi-Core Concurrency:** Worker Isolate Pool distributes CPU-heavy workloads across pre-warmed background Lua states.
* **Custom Native Menu Bar:** Object-oriented menus compiling into native OS menu bar items (Cocoa `NSMenu` on macOS, Win32 `HMENU` on Windows, and GTK3 `GtkMenuBar` on Linux).

---

## Platform & Capability Support Matrix

The table below documents the verification status and implementation state of capabilities shipped in this repository:

| Capability / Subsystem | macOS (WebKit) | Linux (WebKitGTK) | Windows (WebView2) | Implementation Details & Validation |
| :--- | :---: | :---: | :---: | :--- |
| **Window & Webview Management** | **Implemented** | **Implemented** | **Experimental** | macOS tested on Sonoma 14+; Linux supports WebKitGTK 4.1 (with 4.0 fallback); Windows requires WebView2 runtime. |
| **Frontend-to-Native IPC** | **Implemented** | **Implemented** | **Implemented (Seams)** | macOS/Linux verified in automated smoke and regression suites; Windows IPC message parser, UTF-16 conversions, and envelope verification validated in native CTest suite. |
| **Origin & Frame Isolation** | **Implemented** | **Implemented** | **Implemented** | macOS enforces `message.frameInfo.isMainFrame`; Windows enforces `is_frame_source_allowed`; Linux enforces navigation policy and strict CSP (`frame-src 'none'`). |
| **Filesystem Access (`fs:*`)** | **Implemented** | **Implemented** | **Implemented** | Deny-by-default for empty scopes; canonical resolution; single (`*`) and recursive (`**`) globs; `fs:remove` requires explicit capability. |
| **Desktop Notifications** | **Implemented** | **Implemented** | **Implemented** | macOS uses `UNUserNotificationCenter` with parameterized fallback; Linux uses `notify-send --`; Windows uses `Shell_NotifyIconW`. |
| **System Theme & Events** | **Implemented** | **Implemented** | **Implemented** | System theme detection and window event dispatching across active windows. |
| **System Tray & Menus** | **Implemented** | **Implemented** | **Implemented** | Native menu bars (Cocoa / GTK3 / Win32) and system tray icon support. |
| **Process Spawning (`shell:execute`)** | **Implemented** | **Implemented** | **Implemented** | Mandatory `allowed_programs` allowlist, argument policy checks, dangerous flag filtering, and 1 MB output cap. |
| **C Embedding (`libtaurilua`)** | **Implemented** | **Implemented** | **Implemented** | C ABI defined in `include/taurilua.h`; validated by `tests/smoke_test.c` and `examples/host_c/`. |
| **Application Updater** | **Not Implemented** | **Not Implemented** | **Not Implemented** | Disabled for security. Unauthenticated archive extraction removed; calls return a fail-closed error. |
| **Companion Plugins (`sql`, `http`, etc.)** | **External** | **External** | **External** | Companion plugins are maintained in the separate [`lua-tauri-plugins`](https://github.com/MoxPy/lua-tauri-plugins) workspace. |

> [!NOTE]
> **Status Definitions:**
> - **Implemented:** Feature has complete implementation in this repository and passes local automated regression tests.
> - **Experimental / Unverified:** Feature has native implementation in source but has not been verified on end-to-end hardware or automated CI in this repository.
> - **Platform-Dependent:** Feature implementation is constrained by underlying OS webview engine capabilities.
> - **Not Implemented:** Feature is intentionally disabled or not yet implemented.

---

## Architecture Overview

```mermaid
flowchart TB
    subgraph Frontend["Frontend Layer (Chromeless Webview)"]
        UI["UI (HTML5 / CSS3 / React / Vite)"]
        Client["window.__TAURI__.core.invoke(cmd, args)"]
    end

    subgraph Bridge["System Webview & IPC Bridge (C++17)"]
        WK["OS Webview Engine (WKWebView / WebView2 / WebKitGTK)"]
        Origin["Origin Validator & Isolation Token Check"]
        Router["Thread-Safe Dispatch Queue"]
    end

    subgraph Backend["Backend Layer (LuaJIT & Native OS)"]
        MainState["Main Lua State (Windowing, Tray, Shortcuts)"]
        Isolates["Worker Isolate Pool (Thread 1..N Pre-warmed VM States)"]
        OS["Native APIs (POSIX, Win32, Cocoa, GTK)"]
    end

    UI --> Client
    Client -->|"Custom Protocol tauri://"| WK
    WK --> Origin
    Origin --> Router
    Router -->|UI Events / Windowing| MainState
    Router -->|Parallel IPC Workloads| Isolates
    MainState --> OS
    Isolates --> OS
```

### Core Runtime vs. Application Space (Separation of Concerns)

Tauri-Lua maintains a strict, decoupled boundary between the **Core Framework Engine** and **User Application Space**:

| Layer | Files / Location | Responsibilities & Lifecycle |
| :--- | :--- | :--- |
| **Core Runtime Engine** | `src/` (C++17) & `src/tauri/init.lua` | **Permanent System Layer.** Contains the OS window host (Cocoa/GTK/Win32), isolation token generator, origin validator, thread-safe Isolate Worker Pool, IPC router, `window.__TAURI__.Channel` streaming pipeline, file-system drivers, and native dialog wrappers. This layer is compiled into the binary and is permanently active. |
| **User Application Space** | `src-tauri/src/main.lua` & `tauri.conf.json` | **Developer Logic.** Where you define application state (`app.state`), register business logic endpoints (`app:register(...)`), and configure capabilities. If you remove or refactor any user command (e.g. removing a streaming demo handler), the underlying engine capabilities remain completely intact and ready for future handlers with zero overhead. |

### Isolate Worker Architecture & Threading Model

Tauri-Lua implements a multi-VM isolate architecture designed for thread safety and responsiveness:

1. **Main UI Thread Affinity:** All UI operations (creating windows, dialogs, menu bars, tray icons, notifications) run strictly on the platform's native main thread (`dispatch_sync` on Cocoa/macOS, GTK main loop on Linux, Win32 message thread on Windows) via `run_on_main_sync`.
2. **Worker Isolation (`__IS_ISOLATE_WORKER__`):** Worker threads run independent, pre-warmed LuaJIT VM states to execute CPU-intensive handlers in parallel. The runtime sets `__IS_ISOLATE_WORKER__ = true` in worker states, preventing duplicate window initialization (`app:create_main_window()`), duplicated tray creation, or redundant plugin hooks.
3. **Serialized Shared State:** The shared state engine (`app.state`) stores primitives and JSON-serialized structured tables guarded by native mutexes. Arbitrary Lua functions or coroutines cannot cross isolate VM boundaries, maintaining memory safety and avoiding race conditions.

---

## When to Use Tauri-Lua

### Ideal Use Cases
1. **Developer Tools & Utilities:** Diagnostics, system monitors, log viewers, device managers, and CLI companion GUIs.
2. **Internal Enterprise Tools:** Operations dashboards, hardware interfaces, and rapid proof-of-concept enterprise applications.
3. **High-Performance Micro-Apps:** Native desktop clients that need rapid startup and run persistently in the system tray with negligible RAM usage.
4. **Embedded & Resource-Constrained Environments:** Embedded Linux devices (Raspberry Pi, industrial tablets) where LLVM/Rust toolchains are too resource-heavy to maintain on-device.
5. **Game Engine & Host Software Embedding (`libtaurilua`):** Providing in-game configuration windows, web overlays, debug inspectors, or modding GUIs directly from C/C++, Godot, Unreal Engine, or SDL2/GLFW with a lightweight shared library.

### When to Stick with Tauri (Rust) or Electron
* **Complex Multi-Threaded Memory Ownership:** If you require compile-time memory safety proofs and strictly typed shared-memory structures across dozens of concurrent native threads.
* **Mobile Targets:** If your primary roadmap necessitates iOS and Android binaries (Tauri-Lua deliberately focuses on Desktop: macOS, Linux, Windows).
* **Deep Chromium Hooks:** If you require low-level internal Chromium DevTools Protocol (CDP) manipulation unavailable in OS-native webviews.

---

## Project Structure

Tauri-Lua adopts the official Tauri layout. The application backend and configurations are strictly separated from frontend assets:

```text
my-app/
├── package.json              # Optional: Frontend dependencies & npm scripts
├── dist/                     # Frontend production build output
│   ├── index.html
│   ├── assets/
│   └── ...
├── src-tauri/                # Native application root
│   ├── tauri.conf.json       # Central declarative configuration
│   ├── icons/                # Multi-resolution icons (.icns, .ico, pngs)
│   └── src/
│       └── main.lua          # Backend entrypoint (equivalent to main.rs)
├── target/                   # Distribution artifacts
│   └── bundle/
│       ├── macos/            # Standalone .app bundle
│       ├── dmg/              # Mountable .dmg installer
│       └── deb/              # Debian / Ubuntu package
├── lua-tauri                 # Unified CLI executable
├── include/                  # Public C Embedding API (taurilua.h)
├── examples/                 # Host embedding examples (C, Game Engines)
└── src/                      # Tauri-Lua C++ Core Engine (libtaurilua shared library)
```

---

## Getting Started

### Prerequisites

* **macOS:** `clang` (Xcode Command Line Tools), `cmake`, `luajit`, `luarocks` (`brew install luajit cmake luarocks`). Native `libcurl` is provided by the OS.
* **Linux (Debian/Ubuntu):**
  - **Core Runtime & Webview:** `build-essential`, `cmake`, `luajit` (`libluajit-5.1-dev`), `libgtk-3-dev`, and WebKitGTK (`libwebkit2gtk-4.1-dev` for modern distributions or `libwebkit2gtk-4.0-dev`).
  - **System Notifications & HTTP:** `libnotify-bin` (for desktop notifications), `libcurl4-openssl-dev` (for HTTP plugin requests).
  - **Package Management:** `luarocks` (for hermetic Lua package synchronization).
  - **Remote SQL Drivers (Optional):** `libpq-dev` (PostgreSQL), `default-libmysqlclient-dev` (MySQL / MariaDB) if using remote database connections with the SQL plugin.
* **Windows:** Visual Studio 2022 C++ Build Tools (MSVC), `cmake`, `luajit`, `luarocks`, Microsoft Edge WebView2 (pre-installed on Windows 10/11).

### Quickstart

#### 1. Obtain and Build the Engine

Clone the repository and compile the native runtime and shared library:

```bash
# Clone repository
git clone https://github.com/MoxPy/lua-tauri.git
cd lua-tauri

# Build runtime engine and shared library
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run automated regression test suite:
ctest --test-dir build --output-on-failure

# (Optional) Install CLI globally to run from any directory:
sudo ln -sf $(pwd)/lua-tauri /usr/local/bin/lua-tauri
```

#### 2. Scaffold and Run an Application

Scaffold a new project in seconds and launch the live development environment:

```bash
# Global CLI usage:
lua-tauri create my-app
cd my-app
lua-tauri dev

# Or relative usage from cloned repository:
./lua-tauri create my-app
cd my-app
../lua-tauri dev

# Package for production (.dmg / .app on macOS, .deb on Linux, standalone on Windows):
lua-tauri build  # (or ../lua-tauri build)
```

---

## The CLI: Developer Workflow

The included `./lua-tauri` CLI provides essential developer workflows inspired by `@tauri-apps/cli`:

### `lua-tauri create [app-name]`
Interactive and automated project scaffolder. Generates a modern Vite frontend pre-wired to `src-tauri/` with zero configuration needed.

```bash
# Interactive mode (asks for Framework, Tailwind CSS, Package Manager):
./lua-tauri create my-desktop-app

# Automated non-interactive mode:
./lua-tauri create my-react-app --template react --tailwind --pm pnpm
./lua-tauri create my-vue-app --template vue --no-tailwind --pm npm
./lua-tauri create my-svelte-app --template svelte --pm bun
./lua-tauri create my-vanilla-app --template vanilla
```

**Supported Options:**
* `--template, -t <vanilla | react | vue | svelte>` — Frontend framework.
* `--tailwind / --no-tailwind` — Pre-configure Tailwind CSS v3 with PostCSS and autoprefixer.
* `--pm, --package-manager <npm | pnpm | bun | yarn>` — Package manager of choice.

### `lua-tauri dev`
Starts the development cycle:
1. Executes `build.beforeDevCommand` (e.g. `npm run dev`) if specified.
2. Polls `build.devUrl` until your frontend dev server (e.g., Vite) is ready.
3. Launches the native desktop runtime pointed to your dev server.
4. Activates the **Backend File Watcher**: any modification in `src-tauri/src/*.lua` or `tauri.conf.json` automatically triggers a hot backend restart.
5. Automatically terminates the development session when the application window is closed by the user.

```bash
./lua-tauri dev
```

### `lua-tauri build`
Generates optimized production distributions:
1. Executes `build.beforeBuildCommand` (e.g. `npm run build`) to produce `frontendDist`.
2. Compiles user scripts and core libraries into bytecode using `luajit -b`.
3. Inlines the bytecode and configuration directly into the compiled native binary.
4. Generates standard platform bundles:
   - **macOS:** `.app` and `.dmg` inside `target/bundle/`
   - **Linux:** `.deb` inside `target/bundle/deb/`
   - **Windows:** Standalone folder with `.exe` in `target/bundle/windows/`

```bash
./lua-tauri build
```

### `lua-tauri rocks [command]`
Manages hermetic, isolated **LuaRocks** dependencies for your application inside `src-tauri/.rocks/`:
* `lua-tauri rocks install [rock] [ver]` — Installs declared rocks from `tauri.conf.json` or an individual rock.
* `lua-tauri rocks list` — Lists all rocks currently installed in the project's hermetic tree.
* `lua-tauri rocks remove <rock>` — Uninstalls a rock from the hermetic tree.
* `lua-tauri rocks clean` — Clears the local `.rocks/` directory (or use `lua-tauri clean --all`).
* `lua-tauri rocks path` — Prints the exact `LUA_PATH` and `LUA_CPATH` exports for the local tree.

```bash
./lua-tauri rocks install inspect
./lua-tauri rocks list
```

### `lua-tauri init`
Scaffolds a `src-tauri/` directory structure with a baseline `tauri.conf.json` and boilerplate `main.lua` into any existing web project.

```bash
./lua-tauri init
```

### `lua-tauri icon <source.png>`
Generates all multi-resolution platform icon sets (`.icns`, `.ico`, 32x32, 128x128) from a single 1024x1024 source image.

```bash
./lua-tauri icon app-icon.png
```

---

## Declarative Configuration: `tauri.conf.json`

The runtime configuration uses a Tauri-inspired declarative schema ([schemas/tauri-lua.schema.json](schemas/tauri-lua.schema.json)):

```json
{
  "$schema": "./schemas/tauri-lua.schema.json",
  "productName": "tauri-lua-app",
  "version": "1.0.0",
  "identifier": "com.tauri.lua",
  "build": {
    "beforeDevCommand": "npm run dev",
    "devUrl": "http://localhost:5173",
    "beforeBuildCommand": "npm run build",
    "frontendDist": "../dist"
  },
  "dependencies": {
    "luarocks": [
      "inspect",
      "luasocket 3.1.0-1",
      { "name": "lua-cjson", "version": "2.1.0" }
    ]
  },
  "app": {
    "windows": [
      {
        "title": "Tauri Lua Production App",
        "width": 1024,
        "height": 720,
        "resizable": true
      }
    ],
    "security": {
      "capabilities": [
        "core:default",
        "fs:read-text-file",
        "fs:write-text-file",
        "dialog:open",
        "dialog:save",
        "clipboard:write-text"
      ],
      "scope": [
        "$APPDATA/*",
        "$DESKTOP/*",
        "$HOME/Documents/*"
      ]
    }
  }
}
```

---

## Backend Programming Model (`main.lua`)

Writing native commands in Lua is simple, expressive, and thread-safe:

```lua
-- src-tauri/src/main.lua
local Tauri = require("tauri")

-- 1. Initialize application with configuration
local conf = {}
if __EMBEDDED_CONFIG_JSON__ then
    conf = Tauri.json.decode(__EMBEDDED_CONFIG_JSON__) or {}
end
local app = Tauri.init(conf)

-- 2. Persistent Thread-Safe Application State
if not app.state:has("counter") then
    app.state:set("counter", 0)
end

-- 3. Register IPC Command (callable from JavaScript via invoke())
app:register("calculate_fibonacci", function(payload)
    -- Input validation: n must be a non-negative integer in [0, 78].
    -- Lua numbers are IEEE-754 doubles (53-bit integer precision);
    -- fib(78) is 5,527,939,700,884,757 < 2^53. Beyond n=78, doubles lose precision.
    local n = tonumber(payload.n)
    if not n or n ~= math.floor(n) or n < 0 or n > 78 then
        error("Invalid input: 'n' must be an integer between 0 and 78")
    end

    local a, b = 0, 1
    for _ = 1, n do
        a, b = b, a + b
    end

    -- Atomic state update across workers and concurrent requests
    local count = app.state:update("counter", function(current)
        return (current or 0) + 1
    end)

    return {
        input = n,
        result = a,
        totalInvocations = count,
        workerId = __ISOLATE_WORKER_ID__ or 0
    }
end)

-- 4. Create native window & start event loop (or app:run())
app:create_main_window()
```

### Frontend Invocation (JavaScript / TypeScript)

Invoke registered commands from any frontend framework using the standard Tauri IPC interface:

```javascript
import { invoke } from '@tauri-apps/api/core'; // Or window.__TAURI__.core.invoke (window.__TAURI__.invoke supported as legacy alias)

async function calculate() {
  try {
    const response = await window.__TAURI__.core.invoke('calculate_fibonacci', { n: 40 });
    console.log('Result from Lua backend:', response.result);
    console.log('Processed by worker:', response.workerId);
  } catch (error) {
    console.error('IPC Error:', error);
  }
}
```

### Real-Time Streaming Channels

Stream live progress, shell logs, or continuous data chunks from Lua to JavaScript without global event bus pollution:

**In Lua Backend (`src-tauri/src/main.lua`):**
```lua
app:register("stream_countdown", function(payload, caller, channel)
    local ch = channel or (payload and payload.onProgress)
    for i = 5, 1, -1 do
        ch:send({ tick = i, message = "Processing step #" .. i })
        Tauri.sleep(200) -- Built-in cross-platform millisecond sleep
    end
    return { status = "completed" }
end)
```

**In JavaScript Frontend:**
```javascript
const channel = new window.__TAURI__.Channel((data) => {
  console.log("Received stream chunk:", data.message);
});

const result = await window.__TAURI__.core.invoke("stream_countdown", {
  onProgress: channel
});
console.log("Stream finished:", result.status);
```

---

## Built-In Desktop APIs

Tauri-Lua provides a comprehensive set of built-in system abstractions accessible from both Lua and JavaScript:

### 1. File System (`fs`)
Guarded by security capabilities (`fs:read-text-file`, `fs:write-text-file`) and sandbox scopes:
* `tauri:fs:read` — Reads UTF-8 file contents.
* `tauri:fs:write` — Atomically writes text to file.
* `tauri:fs:list` — Non-blocking directory traversal.
* `tauri:fs:remove` — File removal within allowed scope.

### 2. Native Dialogs (`dialog`)
System-native modals matching host OS design tokens:
* `tauri:dialog:open` — Native file/folder selection picker.
* `tauri:dialog:save` — Save file destination picker.
* `tauri:dialog:message` — Native informational message box.
* `tauri:dialog:ask` — Native confirmation dialog (Yes / No).

### 3. System Tray (`tray`)
* `tauri:tray:create` — Instantiates a status bar / tray icon with custom tooltip.
* Tray item click dispatches natively to `__TAURI_DISPATCH_TRAY__(id)`.

### 4. Global Shortcuts & Deep Links
* `tauri:shortcut:register` — Binds system-wide hotkeys (e.g. `CmdOrCtrl+Shift+K`).
* `tauri:deep-link` — Handles custom protocol callbacks (`my-app://action?token=...`).

### 5. Multi-Window Management & Window Lifecycle Events
* **Programmatic Creation:** `app:register("open_window", ...)` -> `tauri:window:create`.
* **Controls:** `minimize`, `maximize`, `unmaximize`, `show`, `hide`, `close`, `destroy`, and custom borderless window drag (`start_dragging`).
* **Off-Screen Geometry Setup:** Windows are sized, styled, and centered off-screen before presentation, eliminating visual resize jumps, flicker, and bounce glitches.

#### Window Lifecycle & Unsaved Data Protection (`prevent_default`)
Essential for production applications (document editors, dashboards, forms) where closing a window inadvertently would result in data loss. Tauri-Lua intercepts the native OS close signal **before** window destruction occurs:
* **macOS:** Cocoa delegate `[windowShouldClose:]` intercepts close and returns `NO` when cancelled.
* **Windows:** Win32 message loop intercepts `WM_CLOSE` and aborts `DestroyWindow`.
* **Linux:** GTK3 connects to `"delete-event"` and returns `TRUE` to halt window destruction.

##### 1. Backend Interception (`Lua`)
```lua
app:on_window_event(function(event)
    if event.event == "close-requested" then
        print("[LIFECYCLE] Window close requested for:", event.label)

        -- 1. Check if there are unsaved changes
        local has_unsaved_data = check_unsaved_changes()

        if has_unsaved_data then
            -- 2. Intercept and block OS destruction
            event:prevent_default()
            print("[LIFECYCLE] Window close cancelled; protecting user data.")

            -- 3. Trigger frontend prompt or native confirmation dialog
            app:emit("prompt-unsaved-changes", { label = event.label })
        end
    end
end)

-- When the user confirms "Discard & Exit", force window destruction:
app:register("confirm_discard_and_close", function(payload)
    app:destroy_window(payload.label) -- Unconditional native close
end)
```

##### 2. Frontend Interception (`JavaScript`)
```javascript
import { getCurrentWebviewWindow } from "@tauri-apps/api/webviewWindow";

const appWindow = getCurrentWebviewWindow();

// Listen for native close requests (clicking X or pressing Cmd+W / Alt+F4)
await appWindow.onCloseRequested(async (event) => {
  if (isEditorDirty) {
    // Prevent the window from closing immediately
    event.preventDefault();

    const saveChanges = await window.__TAURI__.dialog.ask(
      "You have unsaved changes. Do you want to save before closing?",
      { title: "Unsaved Changes", kind: "warning" }
    );

    if (saveChanges) {
      await saveDocument();
      await appWindow.destroy(); // Force close after saving
    } else {
      const forceDiscard = await window.__TAURI__.dialog.ask("Discard all changes and exit?");
      if (forceDiscard) {
        await appWindow.destroy(); // Force close without saving
      }
    }
  }
});
```


### 6. Hardened Process Execution (`shell`)
Direct kernel-level process spawning with zero shell interpolation and real-time streaming:
* `tauri:shell:execute` — Spawns processes directly via `posix_spawnp` (macOS/Linux) or `CreateProcessW` (Windows) using discrete `argv` arrays.

**Frontend Usage (`JavaScript`):**
```javascript
// 1. Discrete argument array (Recommended: immune to command injection)
// Note: requires "shell:execute" capability and "ping" in allowed_programs in tauri.conf.json
await window.__TAURI__.shell.execute("ping", ["-c", "3", "127.0.0.1"], (chunk) => {
  console.log("Live stdout:", chunk.line);
});

// 2. Separate program + args (tokenized without invoking /bin/sh)
// Note: single-string form is tokenized safely, no /bin/sh is ever invoked
const result = await window.__TAURI__.shell.execute("git", ["status", "-s"]);
console.log("Exit code:", result.code);
console.log("Output:\n", result.stdout);
```

> **Security note:** `shell:execute` requires the `"shell:execute"` capability in `tauri.conf.json`.
> Use the `allowed_programs` scope to restrict which executables (and optionally which arguments) are permitted.

### 7. Custom Native Menu Bar API

Tauri-Lua provides an object-oriented menu API inspired by the **Tauri 2 (`muda`) Menu Specification** directly in Lua and C++17, compiling into native OS menu bar items (**Cocoa `NSMenu`** on macOS, **Win32 `HMENU`** on Windows, and **GTK3 `GtkMenuBar`** on Linux) without external dependencies.

#### Architecture Highlights
* **Native OS Responder Chain:** Standard items (`undo`, `copy`, `paste`, `quit`, `minimize`, etc.) are wired directly to native OS handlers (Cocoa selectors on macOS, `WM_COMMAND` / `WM_CLOSE` on Windows, and GTK window actions on Linux) requiring zero manual JS/Lua code to handle clipboard and window shortcuts.
* **Dual-Channel Event Dispatching:** Custom items dispatch simultaneously to the Lua backend (`app:on_menu(cb)`) and the frontend webview (`window.__TAURI__.menu.onMenuClicked(cb)` and `tauri://menu-event`).
* **OOP Builders & Declarative Tables:** Supports both fluent OOP builder classes (`Tauri.Menu`, `Tauri.Submenu`, etc.) and plain declarative Lua tables.

#### Component Classes Reference

| Class | Constructor | Description |
| :--- | :--- | :--- |
| `Tauri.Menu` | `Menu.new(options)` | Root menu bar container. Also supports `Menu.default()` to generate standard OS menus. |
| `Tauri.Submenu` | `Submenu.new(options)` | Nested dropdown menu (e.g. *File*, *Edit*, *Window*). |
| `Tauri.MenuItem` | `MenuItem.new(options)` | Standard clickable menu item with unique `id`, `text`, and optional `accelerator`. |
| `Tauri.CheckMenuItem` | `CheckMenuItem.new(options)` | Toggleable menu item with checkmark state (`checked = true/false`). |
| `Tauri.PredefinedMenuItem` | `PredefinedMenuItem.<role>()` | Built-in system items mapped to native operating system roles. |

#### Predefined OS Roles (`PredefinedMenuItem`)

| Helper Method | Role Identifier | Default Accelerator | Native OS Action (macOS / Windows / Linux) |
| :--- | :--- | :--- | :--- |
| `PredefinedMenuItem.separator()` | `separator` | — | `NSMenuItem separatorItem` / `MF_SEPARATOR` / `GtkSeparatorMenuItem` |
| `PredefinedMenuItem.undo([text])` | `undo` | `CmdOrCtrl+Z` | `undo:` / `WM_UNDO` / Undo action |
| `PredefinedMenuItem.redo([text])` | `redo` | `CmdOrCtrl+Shift+Z` | `redo:` / Redo action |
| `PredefinedMenuItem.cut([text])` | `cut` | `CmdOrCtrl+X` | `cut:` / `WM_CUT` / Cut action |
| `PredefinedMenuItem.copy([text])` | `copy` | `CmdOrCtrl+C` | `copy:` / `WM_COPY` / Copy action |
| `PredefinedMenuItem.paste([text])` | `paste` | `CmdOrCtrl+V` | `paste:` / `WM_PASTE` / Paste action |
| `PredefinedMenuItem.select_all([text])` | `select_all` | `CmdOrCtrl+A` | `selectAll:` / Select all action |
| `PredefinedMenuItem.minimize([text])` | `minimize` | `CmdOrCtrl+M` | `performMiniaturize:` / `SW_MINIMIZE` / `gtk_window_iconify` |
| `PredefinedMenuItem.zoom([text])` | `zoom` | — | `performZoom:` / `SW_MAXIMIZE` / `gtk_window_maximize` |
| `PredefinedMenuItem.fullscreen([text])` | `fullscreen` | `Ctrl+Cmd+F` | `toggleFullScreen:` / Fullscreen Toggle / `gtk_window_fullscreen` |
| `PredefinedMenuItem.hide([text])` | `hide` | `CmdOrCtrl+H` | `hide:` / `ShowWindow(SW_HIDE)` / Minimize to tray |
| `PredefinedMenuItem.hide_others([text])` | `hide_others` | `Alt+Cmd+H` | `hideOtherApplications:` / OS application hide |
| `PredefinedMenuItem.show_all([text])` | `show_all` | — | `unhideAllApplications:` / Unhide all |
| `PredefinedMenuItem.close_window([text])`| `close_window`| `CmdOrCtrl+W` | `performClose:` / `WM_CLOSE` / `gtk_window_close` |
| `PredefinedMenuItem.quit([text])` | `quit` | `CmdOrCtrl+Q` | `terminate:` / `PostQuitMessage` / `gtk_main_quit` |
| `PredefinedMenuItem.about([text])` | `about` | — | `orderFrontStandardAboutPanel:` / About dialog |

#### Full Implementation Example

##### 1. Backend (`Lua`)
```lua
local Tauri = require("tauri")
local app = Tauri.init(conf)

local Menu = Tauri.Menu
local Submenu = Tauri.Submenu
local MenuItem = Tauri.MenuItem
local CheckMenuItem = Tauri.CheckMenuItem
local PredefinedMenuItem = Tauri.PredefinedMenuItem

-- Compose application menu bar
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

-- Assign menu to the application
app:set_menu(menu) -- Or use: app:set_menu(Menu.default())

-- Handle custom menu click events in Lua
app:on_menu(function(event)
    print("[LUA MENU EVENT] Clicked item:", event.id)
    if event.id == "new_project" then
        -- Trigger backend logic, open dialogs, etc.
    end
end)
```

##### 2. Frontend (`JavaScript`)
```javascript
// Method A: Dedicated menu callback
window.__TAURI__.menu.onMenuClicked((event) => {
  console.log("Native menu clicked:", event.id);
});

// Method B: Standard Tauri event bus
window.__TAURI__.event.listen("tauri://menu-event", (event) => {
  console.log("Menu event received:", event.payload.id);
});

// Method C: Dynamically update or set menu from frontend
await window.__TAURI__.menu.setMenu({
  items: [
    {
      text: "Tools",
      items: [
        { id: "export_pdf", text: "Export to PDF...", accelerator: "CmdOrCtrl+E" }
      ]
    }
  ]
});
```

### 8. Single-Instance Application Lock

Tauri-Lua guarantees that only a single instance of your application runs at any time, preventing duplicate windows, split-brain state, or file database corruption:

* **POSIX (macOS & Linux):** Manages a dedicated Unix domain socket placed securely in `$XDG_RUNTIME_DIR` or `~/.local/run/` with permissions `0700` (explicitly refusing insecure shared directories like `/tmp`) with automatic stale socket cleanup.
* **Windows (Win32):** Operates a system-wide Named Pipe (`\\.\pipe\<app_id>-single-instance`).
* **Instant Handshake & Forwarding:** When a user opens a second instance (e.g. clicking the app icon again or double-clicking an associated file):
  1. The second instance forwards its CLI `argv` arguments and `cwd` to the running primary instance.
  2. The primary instance automatically unminimizes and brings its main window to the front (`platform_window_set_focus`).
  3. The primary instance emits `tauri://single-instance` to the frontend and triggers backend Lua handlers.
  4. The second instance exits cleanly with exit code `0` before initializing any webview or isolate overhead.

#### Configuration (`tauri.conf.json`)
```json
{
  "plugins": {
    "single-instance": {
      "enabled": true
    }
  }
}
```

#### 1. Backend Usage (`Lua`)
```lua
local app = Tauri.init(conf)

-- Listen for second instance launch events and received arguments
app:on_single_instance(function(data)
    print("[SINGLE-INSTANCE] Second instance attempted launch with arguments:")
    for i, arg in ipairs(data.args) do
        print(string.format("  arg[%d] = %s", i, arg))
    end
    print("[SINGLE-INSTANCE] Working directory:", data.cwd)

    -- Focus window and notify the user
    app:set_window_focus("main")
    app:notify("App Already Running", "Switched focus to existing window.")
end)
```

#### 2. Frontend Usage (`JavaScript`)
```javascript
// Method A: Dedicated Single-Instance plugin hook
window.__TAURI__.singleInstance.onSecondInstance((data) => {
  console.log("Second instance launched with CLI args:", data.args);
  console.log("Originating working directory:", data.cwd);
  
  if (data.args.includes("--open-file")) {
    const fileIndex = data.args.indexOf("--open-file") + 1;
    openDocument(data.args[fileIndex]);
  }
});

// Method B: Standard Tauri event listener
window.__TAURI__.event.listen("tauri://single-instance", (event) => {
  console.log("Single-instance event:", event.payload);
});
```

---

### 9. Native Desktop Notifications

Tauri-Lua delivers non-blocking, operating-system-native desktop notifications with direct process spawning:

* **macOS:** Modern Apple **`UserNotifications`** framework (`UNUserNotificationCenter`, `UNNotificationRequest`, and `ModernNotificationCenterDelegate` displaying banners even when foregrounded) with automatic authorization requests in packaged mode, and a parameterized `osascript` argv fallback (`on run argv`, avoiding shell/script string interpolation) for unpackaged terminal development.
* **Windows:** Native notifications dispatched using `Shell_NotifyIconW` buffer structures.
* **Linux:** Non-blocking `posix_spawnp` of `notify-send` with discrete `argv` arrays (zero shell interpolation).

#### 1. Backend Usage (`Lua`)
```lua
-- Simple shorthand
app:notify("Download Completed", "All assets have been synchronized.")

-- Or with configuration table
app:notify({
    title = "Compilation Finished",
    body = "Binary compiled successfully in 1.4s."
})

-- Global helper
Tauri.notification.send("System Update", "New version ready to install.")
```

#### 2. Frontend Usage (`JavaScript`)
```javascript
// Check and request permission (matches @tauri-apps/plugin-notification)
const isGranted = await window.__TAURI__.notification.isPermissionGranted();
if (!isGranted) {
  const permission = await window.__TAURI__.notification.requestPermission();
  console.log("Notification permission:", permission);
}

// Send native notification
await window.__TAURI__.notification.sendNotification({
  title: "Cloud Backup",
  body: "Project was successfully backed up to cloud storage."
});
```

---

### 10. Real-Time OS System Theme Detection

Tauri-Lua continuously tracks the host operating system's appearance mode and informs both backend and frontend immediately when the user toggles dark or light mode in system settings:

* **macOS:** Evaluates `[NSApp effectiveAppearance]` / `AppleInterfaceStyle` and observes `AppleInterfaceThemeChangedNotification` via `NSDistributedNotificationCenter`.
* **Windows:** Queries `AppsUseLightTheme` in `HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize` and listens to `WM_SETTINGCHANGE`.
* **Linux:** Queries `gtk-theme-name` / `gtk-application-prefer-dark-theme` and listens to `notify::gtk-theme-name`.

#### 1. Backend Usage (`Lua`)
```lua
-- Query current theme ("dark" or "light")
local current_theme = app:get_theme()
print("[THEME] Current OS appearance:", current_theme)

-- Listen for real-time appearance changes
app:on_theme_changed(function(theme)
    print("[THEME] System theme toggled to:", theme)
end)
```

#### 2. Frontend Usage (`JavaScript`)
```javascript
// Method A: Query theme programmatically
const theme = await window.__TAURI__.window.theme();
console.log("Current theme:", theme); // "dark" | "light"

// Method B: Listen to live OS theme change events
window.__TAURI__.event.listen("tauri://theme-changed", (event) => {
  console.log("New system theme:", event.payload.theme);
  document.documentElement.setAttribute("data-theme", event.payload.theme);
});
```

---

### 11. Native File Drag & Drop

Provides native desktop file drag & drop into webview windows, extracting the **real absolute filesystem paths** without browser sandbox restrictions:

* **macOS:** Swizzles `performDragOperation:` on `WKWebView` to capture `NSPasteboardTypeFileURL`, converting window coordinates and suppressing browser page navigation.
* **Windows:** Registers `DragAcceptFiles` and processes `WM_DROPFILES` via `DragQueryFileW`.
* **Linux:** Configures `gtk_drag_dest_set` and listens to `"drag-data-received"`.

#### 1. Backend Usage (`Lua`)
```lua
app:on_file_drop(function(event)
    print(string.format("[FILE DROP] Window '%s' received event: %s", event.label, event.type))
    print(string.format("  Drop coordinates: x=%d, y=%d", event.position.x, event.position.y))
    for i, path in ipairs(event.paths) do
        print(string.format("  File #%d: %s", i, path))
    end
end)
```

#### 2. Frontend Usage (`JavaScript`)
```javascript
// Method A: Dedicated window drag & drop handler
import { getCurrentWebviewWindow } from "@tauri-apps/api/webviewWindow";

const appWindow = getCurrentWebviewWindow();
appWindow.onDragDropEvent((event) => {
  if (event.type === "drop") {
    console.log("User dropped files:", event.paths);
    console.log("Position:", event.position.x, event.position.y);
  }
});

// Method B: Global event bus listener
window.__TAURI__.event.listen("tauri://drag-drop", (event) => {
  console.log("Dropped paths:", event.payload.paths);
});
```

---

### 12. Advanced Native Clipboard (Text, HTML, Images & Clear)

Comprehensive clipboard API supporting rich HTML and Base64-encoded image bitmaps:

* **Text:** UTF-8 string read and write (`NSPasteboardTypeString`, `CF_TEXT`, `GtkClipboard`).
* **HTML:** Rich formatted markup (`NSPasteboardTypeHTML`, Windows `CF_HTML`, Linux `text/html`).
* **Images:** PNG and TIFF bitmaps converted directly to/from Base64 (`NSPasteboardTypePNG`, Windows `CF_DIB`).
* **Clear:** Clears the system clipboard atomically.

#### 1. Backend Usage (`Lua`)
```lua
-- Plain text
app.clipboard:write_text("Copied from Lua backend")
local text = app.clipboard:read_text()

-- Rich HTML
app.clipboard:write_html("<strong>Rich Text Content</strong>")
local html = app.clipboard:read_html()

-- Image (Base64 PNG)
app.clipboard:write_image("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=")
local base64_png = app.clipboard:read_image()

-- Clear clipboard
app.clipboard:clear()
```

#### 2. Frontend Usage (`JavaScript`)
```javascript
// 1. Text
await window.__TAURI__.clipboard.writeText("Copied from webview!");
const text = await window.__TAURI__.clipboard.readText();

// 2. Rich HTML
await window.__TAURI__.clipboard.writeHtml("<h1>Formatted Header</h1>");
const html = await window.__TAURI__.clipboard.readHtml();

// 3. Image (Base64 data URL or raw Base64 string)
await window.__TAURI__.clipboard.writeImage("data:image/png;base64,iVBORw0KGgo...");
const imageBase64 = await window.__TAURI__.clipboard.readImage();

// 4. Clear
await window.__TAURI__.clipboard.clear();
```

---

## Plugin System & Extensibility

Tauri-Lua implements a decoupled, first-class **Plugin Architecture** matching the design patterns of [Tauri 2 plugins](https://github.com/tauri-apps/plugins-workspace). The core runtime remains lean and bloat-free, while domain-specific capabilities can be loaded modularly from the official [**`lua-tauri-plugins`**](https://github.com/MoxPy/lua-tauri-plugins) workspace.

```mermaid
flowchart LR
    subgraph Host["Tauri-Lua Host App"]
        App["app = tauri.init()"]
        App -->|"app:plugin()"| Registry["Plugin Registry"]
    end

    subgraph Plugins["Official Plugins (lua-tauri-plugins)"]
        SQL["plugin-sql (SQLite + Migrations)"]
        HTTP["plugin-http (libcurl + Scopes)"]
        Autostart["plugin-autostart (LaunchAgent / Registry)"]
        WindowState["plugin-window-state"]
    end

    Registry --> SQL
    Registry --> HTTP
    Registry --> Autostart
    Registry --> WindowState

    SQL -->|"IPC: plugin:sql:exec"| Webview["Client Webview"]
    HTTP -->|"IPC: plugin:http:fetch"| Webview
    Autostart -->|"IPC: plugin:autostart:enable"| Webview
    WindowState -->|"Lifecycle: on_window_close"| Webview
```

### 1. Official Plugin Catalog

All official plugins are maintained in the sibling repository [`lua-tauri-plugins`](https://github.com/MoxPy/lua-tauri-plugins):

| Plugin | Features & Security Architecture | Implementation Status | Supported Drivers / Backends |
| :--- | :--- | :---: | :--- |
| [**`sql`**](https://github.com/MoxPy/lua-tauri-plugins/tree/main/plugins/sql) | **Multi-Database SQL Engine:** Transactional schema migrations (`_sqlx_migrations`), prepared statements with parameterized binds (`$1, $2` and `?`) against SQL injection, and automatic AppData sandboxing. | External (`lua-tauri-plugins`) | **SQLite3** (`sqlite:...`), **PostgreSQL** (`postgres://...`), **MySQL** (`mysql://...`) |
| [**`http`**](https://github.com/MoxPy/lua-tauri-plugins/tree/main/plugins/http) | **Native `libcurl` Client:** Complete CORS bypass for desktop frontends, native OS SSL/TLS certificate validation, connection pooling, and strict `allow`/`deny` URL scope enforcement. | External (`lua-tauri-plugins`) | Native `libcurl` (HTTP/1.1, HTTP/2) |
| [**`autostart`**](https://github.com/MoxPy/lua-tauri-plugins/tree/main/plugins/autostart) | **Launch at System Login:** macOS LaunchAgent `.plist` (`~/Library/LaunchAgents`), Linux XDG Autostart (`~/.config/autostart`), and Windows User Registry (`HKCU\...\Run`). | External (`lua-tauri-plugins`) | Native OS Startup Handlers |
| [**`window-state`**](https://github.com/MoxPy/lua-tauri-plugins/tree/main/plugins/window-state) | **Window State Persistence:** Automatically remembers and restores window dimensions, coordinates, and maximize/fullscreen states across restarts via `on_window_close` hook. | External (`lua-tauri-plugins`) | OS Window Rect & State Cache |

---

### 2. Module Auto-Discovery (`package.path` & `package.cpath`)

The runtime automatically configures Lua search paths to locate plugins without tedious relative paths or environment variables:

| Location | Path Pattern | Use Case |
| :--- | :--- | :--- |
| **External Monorepo** | `../lua-tauri-plugins/plugins/?/init.lua` | Sibling repository for development (active when `TAURILUA_DEV=1`) |
| **Project Local** | `./plugins/?.lua` & `./plugins/?/init.lua` | Project-specific or private plugins |
| **Node Modules** | `./node_modules/@tauri-apps/plugin-?/init.lua` | Installed via npm/pnpm/yarn |
| **Native Binaries** | `?.dylib` (macOS), `?.so` (Linux), `?.dll` (Windows) | Compiled C/C++ FFI extensions |

> [!NOTE]
> Relative sibling paths (`../lua-tauri-plugins/plugins/?/init.lua`) are active during local development mode when `TAURILUA_DEV=1`. In production distributions, plugins must be bundled into application resources or installed via `.rocks/`.

---

### 3. Registering & Using Plugins (`app:plugin`)

```lua
local tauri = require("tauri")
local app = tauri.init()

-- 1. Load Plugins by Name (Auto-Discovered)
local sql = require("sql")
local http = require("http")
local autostart = require("autostart")
local window_state = require("window-state")

-- 2. Configure SQL with Schema Migrations
app:plugin(sql.init({
    default_path = "sqlite:app.db",
    migrations = {
        {
            version = 1,
            description = "create_users_table",
            sql = "CREATE TABLE users (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, email TEXT);"
        }
    }
}))

-- 3. Configure HTTP with Security Scopes
app:plugin(http.init({
    scopes = {
        allow = { "https://api.github.com/*", "https://*.myapp.com/*" },
        deny = { "https://api.github.com/admin/*" }
    }
}))

-- 4. Enable Autostart & Window State
app:plugin(autostart.init())
app:plugin(window_state.init())

app:run()
```

---

### 4. Frontend Usage (`JavaScript / TypeScript`)

Frontend code interacts with plugins using standard Tauri 2 `@tauri-apps/plugin-*` guest APIs or pre-injected globals:

```javascript
// 1. SQL Plugin (@tauri-apps/plugin-sql compatible API)
const db = await window.__TAURI__.sql.Database.load('sqlite:app.db');
await db.execute('INSERT INTO users (name, email) VALUES ($1, $2);', ['Alice', 'alice@company.com']);
const users = await db.select('SELECT * FROM users WHERE email = $1;', ['alice@company.com']);
console.table(users);

// 2. HTTP Plugin (CORS Bypassed)
const response = await window.__TAURI__.http.fetch('https://api.github.com/zen');
if (response.ok) {
  console.log("GitHub Zen:", await response.text());
}

// 3. Autostart Plugin
const isAuto = await window.__TAURI__.autostart.isEnabled();
if (!isAuto) {
  await window.__TAURI__.autostart.enable();
}

// 4. Window State Plugin
const savedState = await window.__TAURI__.windowState.restore('main');
console.log("Restored dimensions:", savedState.width, savedState.height);
```

---

### 5. Plugin Deep Dive: Multi-Database SQL Engine (@tauri-apps/plugin-sql Compatible)

The Tauri-Lua SQL plugin provides an API largely compatible with `@tauri-apps/plugin-sql`. It supports **SQLite**, **PostgreSQL**, and **MySQL / MariaDB** under a unified, declarative connection string interface.

```text
       Frontend Webview (JavaScript / TypeScript)
       window.__TAURI__.sql.Database.load(url)
                          │
       ┌──────────────────┼──────────────────┐
       ▼                  ▼                  ▼
  sqlite:app.db    postgres://...      mysql://...
  (Embedded FFI)   (Remote Cluster)   (Remote Cluster)
       │                  │                  │
       └──────────────────┼──────────────────┘
                          │
                _sqlx_migrations
         (Atomic Versioned Schema Track)
                          │
             Parameterized Bindings ($1, ?)
           (OWASP CWE-89 Parameter Binding)
```

#### Supported Database URI Formats
1. **SQLite (`sqlite:path/to/db.db` or `sqlite:app.db`):**
   - Relative paths like `sqlite:app.db` are automatically sandboxed into the application's native OS AppData directory (`~/Library/Application Support/` on macOS, `~/.local/share/` on Linux, `%APPDATA%` on Windows).
   - Zero external drivers required: operates via native C SQLite3 FFI with high-speed memory mapping.
2. **PostgreSQL (`postgres://user:password@host:port/database` or `postgresql://...`):**
   - Connects to remote or local PostgreSQL clusters with full transaction support.
   - Ideal for internal enterprise tools or multi-user desktop clients syncing with a central database.
3. **MySQL / MariaDB (`mysql://user:password@host:port/database`):**
   - Connects to remote or local MySQL and MariaDB servers.

#### Schema Migrations (`_sqlx_migrations`)
Schema migrations ensure that your local and remote database tables stay in sync across application upgrades:
- Migrations are declared as an array of versioned objects (`version`, `description`, `sql`).
- The runtime maintains an internal `_sqlx_migrations` table tracking applied version numbers, execution checksums, and execution timestamps.
- **Transactionality Note:** On **SQLite** and **PostgreSQL**, schema migrations run within an atomic transaction. On **MySQL / MariaDB**, SQL DDL statements (`CREATE TABLE`, `ALTER TABLE`) trigger an implicit engine commit and cannot be rolled back. If a MySQL migration fails partially midway, the migration entry is marked with a dirty/failed status requiring manual correction before subsequent migrations can proceed.

#### Parameterized Query Binding (Mitigating SQL Injection / CWE-89)
All driver queries support parameterized bindings using positional placeholders (`$1, $2, $3`) or standard question marks (`?`). Prepared statements bind caller parameters separately from the SQL statement grammar:

```javascript
// Safe parameterized query across all engines:
const users = await db.select(
  'SELECT id, name, email FROM users WHERE department = $1 AND active = $2;',
  ['Engineering', true]
);
```

#### Full End-to-End Example

**Backend (`src-tauri/src/main.lua`):**
```lua
local tauri = require("tauri")
local sql = require("sql")
local app = tauri.init()

-- Configure database with automatic forward migrations
app:plugin(sql.init({
    default_path = "sqlite:production.db",
    migrations = {
        {
            version = 1,
            description = "init_schema",
            sql = [[
                CREATE TABLE IF NOT EXISTS users (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    name TEXT NOT NULL,
                    email TEXT UNIQUE NOT NULL,
                    created_at DATETIME DEFAULT CURRENT_TIMESTAMP
                );
            ]]
        },
        {
            version = 2,
            description = "add_department_column",
            sql = "ALTER TABLE users ADD COLUMN department TEXT DEFAULT 'General';"
        }
    }
}))

app:run()
```

**Frontend (`src/main.js` or React / Vue):**
```javascript
// 1. Connect to SQLite or remote PostgreSQL / MySQL
const db = await window.__TAURI__.sql.Database.load('sqlite:production.db');
// Or remote: const db = await window.__TAURI__.sql.Database.load('postgres://postgres:secret@localhost:5432/mycorp');

// 2. Insert with parameterized values
await db.execute(
  'INSERT INTO users (name, email, department) VALUES ($1, $2, $3);',
  ['Alice Rossi', 'alice@company.com', 'Cybersecurity']
);

// 3. Query records safely
const engineeringTeam = await db.select(
  'SELECT * FROM users WHERE department = $1 ORDER BY id DESC;',
  ['Cybersecurity']
);

console.table(engineeringTeam);

// 4. Close database connection when finished
await db.close();
```

---

### 6. CLI Hot-Reload Integration

The development server (`./lua-tauri dev`) actively watches both `./plugins/` and `../lua-tauri-plugins/plugins/`. Any changes saved in your plugin files immediately trigger hot-reloading of the backend runtime.

---

## Hermetic LuaRocks Integration & Dependency Ecosystem

Tauri-Lua features seamless, first-class integration with the global **LuaRocks** package ecosystem. Rather than polluting global system directories (`/usr/local` or `~/.luarocks`), dependencies are managed with **hermetic isolation** inside your project tree (`src-tauri/.rocks/`) and automatically packaged into release bundles.

```text
                    tauri.conf.json
              [dependencies.luarocks]
                         │
                         ▼
             Hermetic Tree Isolation
               (src-tauri/.rocks/)
            ├── share/lua/5.1/  (Lua modules)
            └── lib/lua/5.1/    (C shared objects: .so / .dylib / .dll)
                         │
       ┌─────────────────┴─────────────────┐
       ▼                                   ▼
Development Mode                     Production Bundling
(Auto-Discovered                     (Packaged in Bundle)
 package.path / package.cpath)       ├── macOS: Contents/Resources/rocks/
                                     ├── Linux: /usr/share/<app>/rocks/
                                     └── Windows: <app>-standalone\rocks\
```

### 1. Declarative Dependencies (`tauri.conf.json`)

Declare your required rocks directly in `tauri.conf.json` using the `dependencies.luarocks` section (an ergonomic engine extension designed by Tauri-Lua to integrate native Lua packages into the bundle workflow). Both pure Lua packages and compiled native C extensions (e.g. `luasocket`, `lua-cjson`, `inspect`, `lsqlite3`) are fully supported:

```json
{
  "$schema": "./schemas/tauri-lua.schema.json",
  "productName": "my-tauri-app",
  "dependencies": {
    "luarocks": [
      "inspect 3.1.3-1",
      "luasocket 3.1.0-1",
      {
        "name": "lua-cjson",
        "version": "2.1.0.10-1"
      },
      {
        "name": "luaossl",
        "version": "20220711-0"
      }
    ]
  }
}
```

*Note: If your project includes a standard `*.rockspec` file in the root or `src-tauri/`, Tauri-Lua automatically detects it and can resolve dependencies directly via `luarocks install --only-deps`.*

### 2. Automatic Synchronization & CLI Commands

- **Zero-Friction Dev & Build:** When you run `./lua-tauri dev` or `./lua-tauri build`, Tauri-Lua checks `src-tauri/.rocks/` and automatically synchronizes any missing dependencies.
- **Dedicated CLI Subcommands:**
  ```bash
  # Synchronize all dependencies declared in tauri.conf.json
  ./lua-tauri rocks install

  # Install an individual rock into the local hermetic tree
  ./lua-tauri rocks install luasocket 3.1.0-1

  # List all installed rocks in the local tree
  ./lua-tauri rocks list

  # Remove a rock from the local tree
  ./lua-tauri rocks remove inspect

  # Clean the hermetic .rocks directory
  ./lua-tauri rocks clean
  # Or clean build artifacts + rocks:
  ./lua-tauri clean --all
  ```

### 3. Production Bundle Packaging

When building for production (`./lua-tauri build`), Tauri-Lua packages the hermetic `.rocks` tree directly into the final platform distributions:
- **macOS (`.app` / `.dmg`):** Inlined into `YourApp.app/Contents/Resources/rocks/`
- **Linux (`.deb`):** Inlined into `/usr/share/<app-name>/rocks/`
- **Windows Standalone:** Inlined into `target/bundle/windows/<app-name>-standalone\rocks\`

### 4. Zero-Config Runtime Resolution

`src/tauri/init.lua` dynamically discovers the host executable path across all platforms (`_NSGetExecutablePath` on macOS, `/proc/self/exe` on Linux, `GetModuleFileNameA` on Windows) and configures `package.path` and `package.cpath` to locate both Lua source modules and compiled C extensions (`.so`, `.dylib`, `.dll`) seamlessly.

Simply `require` your rock anywhere in backend Lua code:
```lua
local inspect = require("inspect")
local socket = require("socket")

print("Backend socket version:", socket._VERSION)
```

### 5. Cybersecurity & Defensive Architecture (OWASP / CWE-78)

- **Command Injection Prevention (CWE-78):** Every rock name, version, and server URL parsed from configuration or CLI arguments is validated against strict alphanumeric whitelists. Any forbidden characters (`;`, `&`, `|`, `$`, backticks, newlines) are rejected immediately prior to command construction.
- **Hermetic Isolation (CWE-829 / CWE-427):** Uses `--tree src-tauri/.rocks` to isolate third-party dependencies from the host environment, completely preventing global system pollution, permission escalation risks, and DLL search-order hijacking.
- **Diagnostic Guidance:** If `luarocks` is not found on the developer's system, Tauri-Lua gracefully prints platform-specific installation instructions (`brew install luarocks`, `apt install luarocks`, etc.) rather than crashing with unhandled shell errors.

---

## Scripting Engine Host & C/C++ Embedding (`libtaurilua`)

Tauri-Lua is not only an application runtime—it is also an **embeddable shared library** (`libtaurilua.dylib` on macOS, `libtaurilua.so` on Linux, `taurilua.dll` on Windows) exposing a clean, zero-overhead **Pure-C API** ([include/taurilua.h](include/taurilua.h)).

This empowers existing C/C++ software, native desktop applications, internal enterprise tools, and game engines (**Godot, Unreal Engine, SDL2, GLFW**) to summon modern web frontends on the fly for:
* In-game configuration windows and HUD overlays
* Modding tools, visual inspectors, and script debuggers
* Launcher screens and account authentication flows
* Native tools needing an elegant webview UI without bundling a heavy Chromium browser runtime

```text
  ┌─────────────────────────────────────────────────────────────┐
  │ Host Application (C / C++ / Game Engine / SDL / Godot / UE)  │
  └──────────────────────────────┬──────────────────────────────┘
                                 │
                     #include <taurilua.h>
                                 │
                                 ▼
  ┌─────────────────────────────────────────────────────────────┐
  │  libtaurilua (.dylib / .so / .dll) Shared Engine Library    │
  │  ├── Pure C ABI (Zero C++ Mangling, Binary Stable)          │
  │  ├── Embedded LuaJIT 2.1 VM + Preloaded `tauri` Core        │
  │  ├── Worker Isolate Pool (Multi-threaded background tasks)   │
  │  └── OS Native Webview Bridge (Cocoa / GTK3 / WebView2)     │
  └──────────────────────────────┬──────────────────────────────┘
                                 │
                                 ▼
                   Native OS Window & Webview UI
```

### 1. Pure C API Header Reference (`include/taurilua.h`)

The header uses standard C99 ABI conventions with exported symbols (`TAURILUA_API`):

```c
#include "taurilua.h"

// 1. Struct configuration
typedef struct taurilua_config {
    const char *app_name;         // App name identifier (default: "tauri-lua-app")
    const char *title;            // Window title (default: "Tauri-Lua Host")
    const char *url;              // Webview URL or relative dist path
    int width;                    // Initial width (default: 1024)
    int height;                   // Initial height (default: 720)
    int resizable;                // 1 if resizable, 0 otherwise (default: 1)
    int dev_mode;                 // 1 for Vite dev server, 0 for production
    const char *config_json;      // Optional JSON configuration string
    const char *custom_script;    // Optional path to .lua backend script
} taurilua_config_t;

// 2. Lifecycle management
void taurilua_config_init(taurilua_config_t *config);
taurilua_app_t *taurilua_create(const taurilua_config_t *config);
taurilua_app_t *taurilua_create_from_file(const char *script_path, const char *config_json);
taurilua_app_t *taurilua_create_from_string(const char *lua_source, const char *config_json);

int  taurilua_run(taurilua_app_t *app);
void taurilua_terminate(taurilua_app_t *app);
void taurilua_destroy(taurilua_app_t *app);

// 3. Interoperability & Host-Engine Dispatch
struct lua_State *taurilua_get_lua_state(taurilua_app_t *app);
int  taurilua_eval_lua(taurilua_app_t *app, const char *lua_code);
void taurilua_eval_js(taurilua_app_t *app, const char *window_label, const char *javascript);
void taurilua_emit(taurilua_app_t *app, const char *event_name, const char *payload_json);

// 4. One-Liner Modal Window Launcher
int taurilua_open_window(const char *title, const char *url, int width, int height);
```

### 2. High-Level Modal Helper (`taurilua_open_window`)

If your game or C++ tool only needs to pop up a settings or config dialog and block until closed:

```c
#include "taurilua.h"

int main() {
    // Spawns a native webview window, runs the event loop, and blocks until closed
    taurilua_open_window("Game Settings", "dist/settings.html", 800, 600);
    return 0;
}
```

### 3. Full Engine Embedding Example ([examples/host_c/main.c](examples/host_c/main.c))

For applications that need custom Lua scripts, bi-directional IPC, or direct `lua_State*` manipulation:

```c
#include <stdio.h>
#include "taurilua.h"

int main(int argc, char **argv) {
    // 1. Initialize configuration struct
    taurilua_config_t cfg;
    taurilua_config_init(&cfg);
    cfg.title = "Custom Host Launcher";
    cfg.width = 1024;
    cfg.height = 768;

    // 2. Instantiate embedded engine
    taurilua_app_t *app = taurilua_create(&cfg);
    if (!app) return 1;

    // 3. Execute Lua logic within the runtime
    taurilua_eval_lua(app, "print('Host initialized backend logic!')");

    // 4. Run the native window loop (blocks until windows close)
    int exit_code = taurilua_run(app);

    // 5. Cleanup resources and isolate threads
    taurilua_destroy(app);
    return exit_code;
}
```

### 4. Game Engine Integration (Godot, Unreal, SDL2)

Because `taurilua` is a standard C shared library, you can link it directly into game projects or GDExtension modules:

#### CMake Configuration (`CMakeLists.txt`)
```cmake
find_library(TAURILUA_LIB taurilua PATHS "${CMAKE_CURRENT_SOURCE_DIR}/lib")
target_include_directories(my_game PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(my_game PRIVATE ${TAURILUA_LIB})

# Ensure runtime discovery (macOS / Linux)
if(APPLE)
    set_target_properties(my_game PROPERTIES INSTALL_RPATH "@executable_path")
elseif(UNIX)
    set_target_properties(my_game PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()
```

#### Compilation & Manual Linking
```bash
# macOS
clang -O2 main.c -Iinclude -Lbuild -ltaurilua -Wl,-rpath,@executable_path/build -o host_app

# Linux
gcc -O2 main.c -Iinclude -Lbuild -ltaurilua -Wl,-rpath,'$ORIGIN/build' -o host_app

# Windows (MSVC)
cl /O2 main.c /Iinclude /link build\taurilua.lib
```

---

## Security Architecture

Tauri-Lua implements a multi-layered **Defense-in-Depth** security model:

1. **Direct Process Spawning (Zero Subshell Injection for Frontend Commands):**
   - Frontend command execution (`shell:execute`) bypasses intermediate subshells (`/bin/sh` or `cmd.exe`) and invokes OS process creation directly (`posix_spawnp` on POSIX, `CreateProcessW` on Windows).
   - Shell control metacharacters (`;`, `&&`, `|`, `$()`, backticks) cannot trigger shell command injection.
   - Output chunks are buffered and streamed to `window.__TAURI__.Channel` in real time, capped at 1 MB to prevent memory exhaustion. (Developer CLI commands such as `lua-tauri dev` execute user-specified build scripts in the developer's shell environment).
2. **Dual-Layer Content Security Policy (CSP):**
   - Automatically injected as an HTTP response header (`Content-Security-Policy`) on custom scheme requests (`tauri://localhost`, `asset://`) and as a dynamic DOM `<meta http-equiv="Content-Security-Policy">` tag in all served HTML documents.
   - Default policy enforces strict script origin restrictions without `'unsafe-inline'` in `script-src` (`default-src 'self' tauri: asset:; script-src 'self' tauri: asset:; style-src 'self' 'unsafe-inline'; ...`), neutralizing Cross-Site Scripting (XSS) vectors.
   - Configurable declaratively in `tauri.conf.json` (`security.csp`) or dynamically via `app:set_csp("...")`.
3. **Session Verification Tokens & Frame Isolation:**
   - A cryptographically random 256-bit session token (64 hex characters) is generated via OS CSPRNG (`SecRandomCopyBytes` on macOS, `/dev/urandom` on Linux, `BCryptGenRandom` on Windows) and attached to `window.__TAURI_TOKEN__` at initialization.
   - **Threat Model Scope:** This token acts as a defense-in-depth barrier against unauthorized child iframes and cross-origin subframes. It is **not** a same-origin security boundary (any malicious script executing within the same-origin document context can access JS window variables).
   - **Frame Isolation:** macOS WebKit verifies `message.frameInfo.isMainFrame`. On platforms where webview frame metadata is unavailable, strict CSP (`frame-src 'none'`) prevents framing attacks.
4. **Native Origin Gating (Authoritative Security Boundary):**
   - The authoritative security boundary is enforced at the native C++ level on every IPC message (`get_native_window_origin` & `OriginValidator::is_allowed`). Requests originating from unapproved origins or external untrusted domains are unconditionally rejected before reaching Lua handlers. Strict Content Security Policy (CSP) reinforces same-origin isolation.
5. **Capability & Program Whitelisting (Deny-by-Default):**
   - Tauri-Lua implements an integrated, declarative capability and scope validation engine directly in `tauri.conf.json` (`security.capabilities`). Sensitive native APIs (filesystem, dialogs, clipboard, shell, notifications) are rejected at the native boundary unless explicitly enabled in configuration.
   - The shell engine enforces a mandatory `allowed_programs` list to restrict command execution exclusively to approved executables and permitted arguments.

### Declarative Capabilities & Permission Matrix (Deny-by-Default)

Tauri-Lua implements a strict **Deny-by-Default** security model for frontend IPC envelopes. If `security.capabilities` is empty or omitted, the frontend is strictly forbidden from executing any host system API. Every IPC invoke must be explicitly authorized:

| Capability Identifier | Description | Default Policy |
| :--- | :--- | :--- |
| `core:default` | Basic window control (`close`, `minimize`, `maximize`, `show`, `hide`, `set_title`, `set_size`, `set_focus`), tray icon, shortcuts, and menus | **Denied** (Requires explicit grant) |
| `fs:read-text-file` | Reading file contents via `tauri:fs:read_text_file` (constrained by `security.scope`) | **Denied** |
| `fs:write-text-file`| Writing file contents via `tauri:fs:write_text_file` | **Denied** |
| `fs:remove` | Deleting files via `tauri:fs:remove` (requires explicit `fs:remove` or `fs:*`) | **Denied** |
| `fs:read-dir` | Directory listing traversal via `tauri:fs:read_dir` | **Denied** |
| `fs:*` | Wildcard grant for all filesystem capabilities (`read-text-file`, `write-text-file`, `remove`, `read-dir`) | **Denied** |
| `dialog:open` | Showing native file / folder open pickers via `tauri:dialog:open` | **Denied** |
| `dialog:save` | Showing native file save destination pickers via `tauri:dialog:save` | **Denied** |
| `dialog:message` | Showing native message and confirmation modals (`dialog:message`, `dialog:ask`) | **Denied** |
| `dialog:*` | Wildcard grant for all dialog capabilities | **Denied** |
| `clipboard:read-text` | Reading plain text and HTML from system clipboard | **Denied** |
| `clipboard:write-text`| Writing plain text, HTML, image data, and clearing clipboard | **Denied** |
| `clipboard:*` | Wildcard grant for all clipboard read/write capabilities | **Denied** |
| `notification:send` | Dispatching system desktop notifications | **Denied** |
| `notification:*` | Wildcard grant for notification APIs | **Denied** |
| `shell:execute` | Spawning and executing external host processes (requires mandatory `allowed_programs` allowlist) | **Denied** |
| `updater` / `updater:*`| Application updater (disabled for security reasons) | **Not Implemented** |
| `http:request` / `http:default` | Executing outgoing HTTP/HTTPS fetch requests via the HTTP plugin (requires URL scopes) | **Denied** |
| `sql:execute` / `sql:default` | Executing SQL queries and mutations via the SQL plugin (requires DB connection scopes) | **Denied** |

> [!IMPORTANT]
> **Deny-by-Default Enforcement:**
> Any IPC request originating from frontend JavaScript that requests an ungranted capability is immediately rejected at the native boundary with an authorization error (`Capability non autorizzata: <permission>`). Backend code in `main.lua` runs with full native privileges.

6. **Path Scope Constraint Enforcement:**
   - Filesystem operations enforce deny-by-default on empty scopes, resolve path aliases (`$APPDATA`, `$DESKTOP`, `$HOME`) canonically to prevent directory traversal (`../`) attacks, evaluate single (`*`) and recursive (`**`) globs, and strictly reject paths containing NUL bytes (`\0`).
7. **Transactional Atomic State Management (`app.state:update`):**
   - In-memory shared state mutations are guarded by an optimistic CAS loop, ensuring user callbacks never hold the native mutex and preventing deadlocks.

---

## Cross-Platform Implementation Details

| Layer | macOS | Linux | Windows |
| :--- | :--- | :--- | :--- |
| **Webview Core** | **WKWebView** (WebKit) | **WebKit2GTK** (WebKitGTK 4.1 / 4.0 fallback) | **WebView2** (Chromium Edge) |
| **Window Host** | Cocoa (`NSWindow`) | GTK3 (`GtkWindow`) | Win32 API (`HWND`) |
| **IPC Mechanism** | `WKScriptMessageHandler` | WebKit User Content Controller | `ICoreWebView2::WebMessageReceived` |
| **Native Menu Bar** | Cocoa (`NSMenu`, `NSMenuItem`) | GTK3 (`GtkMenuBar`, `GtkMenuItem`) | Win32 API (`HMENU`, `WM_COMMAND`) |
| **Event Loop** | `[NSApp run]` | `gtk_main()` | `GetMessage` / `DispatchMessage` |
| **Bundle Target** | `.app` & `.dmg` (`hdiutil`) | `.deb` (`dpkg-deb`) | Standalone Folder / Archive |

### Linux Wayland Compatibility & Graceful Degradation

Tauri-Lua runs seamlessly on both X11 and Wayland environments through WebKit2GTK and GTK3. Under Wayland, the security and architecture model places intentional restrictions on client applications:

1. **Window Positioning:** The Wayland protocol disallows client applications from arbitrarily placing windows at fixed screen coordinates. Window positioning calls degrade gracefully as non-fatal no-ops without crashing.
2. **Window State Persistence (`plugin:window-state`):** Window width, height, maximized, and fullscreen states are fully restored. Screen (x, y) coordinates are skipped under Wayland to respect compositor tile/window placement.
3. **Global Shortcuts:** Direct global key grabbing (`XGrabKey`) is prohibited by Wayland compositors for anti-keylogging security. Tauri-Lua degrades cleanly without crashing when running under `WAYLAND_DISPLAY`.

---

## Packaging & Distribution Guide

Distributing desktop applications across heterogeneous operating systems requires packaging native dependencies, dynamic linkers, and asset bundles appropriately.

```text
target/bundle/
├── macos/
│   └── tauri-lua-app.app           # Self-contained macOS Application Bundle
│       └── Contents/Resources/
│           ├── dist/               # Webview frontend assets
│           └── rocks/              # Bundled hermetic LuaRocks & C modules
├── dmg/
│   └── tauri-lua-app_1.0.0_universal.dmg # Mountable Apple Disk Image (or _arm64 / _x86_64)
├── deb/
│   └── tauri-lua-app_1.0.0_amd64.deb # Debian / Ubuntu package (or _arm64 for ARM / Raspberry Pi)
│       └── usr/share/<app-name>/
│           ├── dist/
│           └── rocks/
└── windows/
    └── tauri-lua-app-standalone/   # Portable Windows distribution
        ├── dist/
        └── rocks/
```

### 1. Linux Distribution (`.deb` & Shared Libraries)

> [!IMPORTANT]
> **Why copying just the bare binary (`build/tauri_app`) to another Linux machine is not enough:**  
> Native GUI applications on Linux dynamically link against system libraries—specifically `glibc`, `libgtk-3`, and `libwebkit2gtk-4.1` (or `libwebkit2gtk-4.0` on older distros). If a target machine has differing shared object versions or lacks development headers, launching the raw binary directly will result in missing library errors (`cannot open shared object file`).

`./lua-tauri build` automatically assembles a standards-compliant **Debian (`.deb`) package** inside `target/bundle/deb/`:
* Places the stripped binary in `/usr/bin/<app-name>`
* Copies the frontend distribution to `/usr/share/<app-name>/`
* Generates a `DEBIAN/control` manifest specifying the target architecture (`amd64`), version, and metadata.

**Installation by End Users:**
```bash
# Clean install with automatic dependency resolution via apt:
sudo apt install ./target/bundle/deb/tauri-lua-app_1.0.0_amd64.deb

# Or via dpkg:
sudo dpkg -i target/bundle/deb/tauri-lua-app_1.0.0_amd64.deb
sudo apt-get install -f # Fixes any missing system dependencies
```

---

### 2. macOS Distribution (`.app` & `.dmg`)

On macOS, running `./lua-tauri build` produces two artifacts:
1. **Application Bundle (`.app`):** Formatted with strict Apple bundle hierarchy:
   * `Contents/MacOS/<app-name>` (Native executable with bytecode inlined)
   * `Contents/Resources/dist/` (Sandboxed webview assets loaded via `[NSBundle resourcePath]`)
   * `Contents/Info.plist` (DPI-awareness, bundle identifiers, and window metadata)
2. **Apple Disk Image (`.dmg`):** A compressed, read-only disk image ready for user distribution generated via `hdiutil`.

#### Automated Code Signing & Hardened Runtime

When you run `./lua-tauri build` on macOS, the CLI handles codesigning automatically using Apple's recommended inside-out approach (signing nested `.dylib` libraries first, then the root `.app` bundle, avoiding the deprecated `--deep` flag).

##### 1. Ad-Hoc Signing (Default for Local Builds & Testing)
If no Apple Developer certificate is specified, the CLI automatically signs the application **ad-hoc** (`codesign --sign -`):
* **Entitlements:** Applies `src-tauri/entitlements.plist` (`com.apple.security.cs.allow-jit` for LuaJIT and `network.client`).
* **Library Validation:** Omits `--options runtime`. Because ad-hoc binaries do not possess an Apple Team ID (`TeamIdentifier=not set`), omitting the runtime flag prevents macOS `dyld` from halting on launch with a `mapping process and mapped file have different Team IDs` error when loading `@rpath/libtaurilua.dylib`.
* The resulting `.app` and `.dmg` run immediately on your local Mac without Gatekeeper team mismatches.

##### 2. Apple Developer ID Signing & Notarization (Production Release)
To sign your application for distribution outside the Mac App Store and pass Apple Notarization, specify your Developer ID certificate in either of two ways:

* **Via Environment Variable:**
  ```bash
  export APPLE_SIGNING_IDENTITY="Developer ID Application: Your Company Name (TEAMID12345)"
  ./lua-tauri build
  ```

* **Via `tauri.conf.json`:**
  ```json
  {
    "bundle": {
      "macOS": {
        "signingIdentity": "Developer ID Application: Your Company Name (TEAMID12345)"
      }
    }
  }
  ```

When a Developer ID is detected (`signingIdentity ~= "-"`):
1. **Hardened Runtime (`--options runtime`)** is automatically enabled on the bundle.
2. The executable, `libtaurilua.dylib`, and any compiled LuaRocks C modules are signed with the same identity, stamping matching Apple Team IDs (`TeamIdentifier=TEAMID12345`).
3. You can verify the bundle and notarize the disk image:
   ```bash
   # Verify signature & Gatekeeper acceptance
   codesign --verify --strict --verbose=4 "target/bundle/macos/tauri-lua-app.app"
   spctl --assess --type execute --verbose "target/bundle/macos/tauri-lua-app.app"

   # Submit DMG to Apple Notary Service
   xcrun notarytool submit "target/bundle/dmg/tauri-lua-app_1.0.0_x64.dmg" \
     --keychain-profile "AC_PASSWORD" --wait

   # Staple notarization ticket
   xcrun stapler staple "target/bundle/dmg/tauri-lua-app_1.0.0_x64.dmg"
   ```

---

### 3. Windows Distribution (Standalone & WebView2)

On Windows, `./lua-tauri build` produces a standalone portable directory in `target/bundle/windows/` containing:
* The native executable (`.exe`) compiled against the Microsoft C++ Runtime.
* Bundled web assets in `/dist`.

**System Requirement:**  
Tauri-Lua on Windows utilizes the official **Microsoft Edge WebView2** runtime. This is pre-installed on Windows 10 (updated versions) and Windows 11. On older systems, users can download the Evergreen Bootstrapper installer provided by Microsoft.

---

### 4. Automated Multi-Platform CI/CD (GitHub Actions)

You don't need three separate physical machines to build for macOS, Linux, and Windows. Commit the following workflow into `.github/workflows/release.yml` to build all platform artifacts simultaneously in the cloud whenever you push a version tag:

```yaml
name: Release Multi-Platform Bundles

on:
  push:
    tags:
      - 'v*'

jobs:
  build:
    name: Build (${{ matrix.platform }})
    runs-on: ${{ matrix.os }}
    strategy:
      fail-fast: false
      matrix:
        include:
          - platform: macos
            os: macos-latest
            artifact: target/bundle/dmg/*.dmg
          - platform: linux
            os: ubuntu-latest
            artifact: target/bundle/deb/*.deb
          - platform: windows
            os: windows-latest
            artifact: target/bundle/windows/*

    steps:
      - name: Checkout Source
        uses: actions/checkout@v4

      # Install platform dependencies
      - name: Install Linux Dependencies
        if: matrix.platform == 'linux'
        run: |
          sudo apt-get update
          sudo apt-get install -y cmake build-essential luajit libluajit-5.1-dev libgtk-3-dev
          sudo apt-get install -y libwebkit2gtk-4.1-dev || sudo apt-get install -y libwebkit2gtk-4.0-dev

      - name: Install macOS Dependencies
        if: matrix.platform == 'macos'
        run: brew install cmake luajit

      - name: Install Windows Dependencies (LuaJIT)
        if: matrix.platform == 'windows'
        shell: cmd
        run: |
          git clone --depth 1 https://github.com/LuaJIT/LuaJIT.git %RUNNER_TEMP%\luajit
          cd %RUNNER_TEMP%\luajit\src
          call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
          call msvcbuild.bat
          echo %RUNNER_TEMP%\luajit\src>> %GITHUB_PATH%
          echo LUAJIT_ROOT=%RUNNER_TEMP%\luajit>> %GITHUB_ENV%

      # Run regression tests before packaging
      - name: Run Regression Tests
        run: |
          cmake -B build -DCMAKE_BUILD_TYPE=Release
          cmake --build build
          ctest --test-dir build --output-on-failure

      # Run frontend & backend build
      - name: Build Application Bundle
        run: |
          chmod +x ./lua-tauri
          ./lua-tauri build

      # Publish binary to GitHub Releases
      - name: Upload Release Asset
        uses: softprops/action-gh-release@v2
        with:
          files: ${{ matrix.artifact }}
        env:
          GITHUB_TOKEN: ${{ secrets.GITHUB_TOKEN }}
```

---

## Known Limitations

Like any native framework balancing performance, security, and portability, Tauri-Lua has intentional architectural boundaries and platform-specific constraints:

1. **Wayland Compositor Restrictions (Linux):**
   - Under Wayland, client applications cannot programmatically place windows at arbitrary screen coordinates. Calls to set window position degrade gracefully as non-fatal no-ops.
   - Global hotkey interception (`XGrabKey`) is prohibited by Wayland compositors for security reasons. Global shortcuts degrade cleanly without crashing when `WAYLAND_DISPLAY` is active.
2. **Mobile Platforms (iOS & Android):**
   - Tauri-Lua is purpose-built for desktop operating systems (macOS, Linux, Windows). Mobile platform compilation (iOS/Android) is not supported.
3. **macOS System Notifications in Unpackaged Mode:**
   - Apple's modern `UserNotifications` framework requires an active application bundle identifier and signed `.app` structure. In unpackaged development mode (`./lua-tauri dev`), notifications fall back to a parameterized `osascript` argv delivery (`on run argv`, avoiding shell/script string interpolation) without registering with macOS Notification Center. Full interactive banner delivery activates once packaged into a `.app` bundle.
4. **MySQL / MariaDB DDL Non-Transactionality:**
   - On MySQL and MariaDB, DDL statements (`CREATE TABLE`, `ALTER TABLE`, `DROP TABLE`) trigger an immediate, implicit engine commit and cannot be rolled back within a transaction. If a migration fails mid-script, the migration table marks the version as failed/dirty, requiring manual resolution before running subsequent migrations.

---

## Systems Engineering Best Practices for Lua Developers

When scaling your desktop application with Tauri-Lua, adhere to these principal design rules:

1. **Leverage the Worker Isolate Pool:** Long-running calculations (compression, crypto, large JSON parsing) should always be delegated to registered commands. They run on the worker thread pool, keeping the main UI thread silky smooth.
2. **Favor LuaJIT FFI over C Lua API:** When calling native platform libraries directly from Lua, use LuaJIT's `ffi` module. FFI calls are JIT-compiled into raw assembly and avoid Lua stack conversion overhead.
3. **Respect Sandbox Scopes:** Never bypass `self.security:assert_scope()` in custom filesystem handlers. Always resolve system roots via `$APPDATA`, `$DESKTOP`, or `$HOME`.
4. **Thread-Safe State Synchronization:** The built-in `app.state` synchronizes individual `get`, `set`, and `delete` operations across worker threads using a native recursive mutex (`std::recursive_mutex`). For composite read-modify-write patterns across concurrent isolates, use the functional `app.state:update(key, callback)` helper to execute atomic, transactional updates without race conditions.

---

## License

MIT License.
