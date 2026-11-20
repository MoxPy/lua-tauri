# Security Policy & Threat Model

> [!NOTE]
> **Security Architecture Overview**
> Tauri-Lua combines LuaJIT with a C++17 native bridge and OS-native webviews. This document describes the implemented threat model, defense-in-depth boundaries, compile-time release fuses, platform verification matrix, and responsible disclosure procedures.

---

## 1. Threat Model & Security Boundaries

### Authoritative Boundary: Native Origin & Navigation Gating
The primary and authoritative security boundary is enforced at the native C++ level on every incoming navigation and IPC message (`SecurityPolicy::is_navigation_allowed` and `OriginValidator::is_allowed`):
* **Release Mode:** Only canonical application origins (`tauri://localhost`, `https://tauri.localhost`) are permitted. All remote web origins, arbitrary localhost ports, backslash authorities, and userinfo-spoofed origins (e.g. `http://localhost:80@attacker.com`) are unconditionally rejected, while legitimate `@` characters in paths/queries (e.g. `/@vite/client`, `?email=a@b.com`) are permitted.
* **Dev Mode (`TAURILUA_DEV=1`):** Navigation is permitted to the configured development server (e.g. `http://localhost:5173`, `http://127.0.0.1:5173`, or `http://[::1]:5173`). Host and port matching are strictly enforced.
* **Compile-Time Release Fuse (`-DTAURILUA_RELEASE_FUSE=ON`):** Production builds produced by `lua-tauri build` compile with `TAURILUA_RELEASE_FUSE`, permanently disabling `TAURILUA_DEV`, `TAURI_LUA_ROOT`, CWD asset resolution, and relative `ffi.load` paths regardless of environment variables at runtime.
* **External Windows:** Windows created with `window:create:external` pointing to untrusted origins do **not** receive `window.__TAURI_TOKEN__`, the client IPC bridge script, or `__TAURI_RAW_INVOKE__` bindings.

### Session Token Scope (`window.__TAURI_TOKEN__`)
* A cryptographically secure 256-bit session token (64 hex characters) is generated on startup via the OS CSPRNG (`arc4random_buf` on macOS, `BCryptGenRandom` on Windows, `getrandom`/`/dev/urandom` on Linux). RNG failures abort the process immediately (fail-closed).
* **Enforcement:** Verified on every native IPC envelope using constant-time comparison (`constant_time_eq`) and the unified strict RFC 8259 C++17 JSON parser (`src/json_parser.hpp`, depth cap `64`, zero infinite loops).
* **Scope & Limitation:** The session token defends against unauthorized subframes and cross-origin contexts; it is **not** a same-origin sandbox against XSS inside the top-level document, which must be defended with strict CSP.

### Frame Isolation & Webview Security Controls
* **macOS (WKWebView):** Enforces `message.frameInfo.isMainFrame`, Objective-C ARC (`-fobjc-arc`), `isKindOfClass:[NSString class]` checks, scoped `TauriWKWebView` subclassing (zero global method swizzling), HTTP CSP + `X-Content-Type-Options: nosniff` headers, and fail-closed `runOpenPanelWithParameters`.
* **Windows (WebView2):** Enforces `SecurityPolicy::is_frame_source_allowed`, `add_FrameNavigationStarting`, `COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS`, fail-closed `WebMessageReceived`/`NavigationStarting`, and a mutex-protected dispatch queue (no raw `LPARAM` pointer dereferencing).
* **Linux (WebKitGTK):** Enforces `decide-policy` fail-closed navigation gating, HTTP CSP + `nosniff` response headers on WebKitGTK 4.1+, `<meta>` CSP injection via linear HTML scanner (no `std::regex` ReDoS), and GLib-owned asset buffers (`g_malloc`/`g_free`).

---

## 2. Capabilities & Principle of Least Privilege

Tauri-Lua enforces a **Deny-by-Default** capability model for frontend IPC:
1. Empty or missing `security.capabilities` denies all frontend IPC invocations to built-in host APIs.
2. Backend Lua code (`main.lua`) executes with host privileges; frontend JavaScript is treated as untrusted.

### Capability Presets & Semantics

| Capability | Scope & Permissions Granted | Explicitly NOT Granted |
| :--- | :--- | :--- |
| **`core:default`** | Window control (`window:create` internal, `close`, `minimize`, `maximize`, `show`, `hide`, `set_title`, `set_size`, `set_focus`), theme querying, event emit/listen, menus, shortcuts, tray, dialogs, paths, store, and read-only shared state (`state:get`). | Does **NOT** grant `state:set`, `state:delete`, `state:clear`, `window:create:external`, `updater:*`, `fs:*`, `clipboard:*`, or `shell:*`. |
| **`state:set`** | Modifying shared backend state from the frontend. | Decoupled from `core:default` and `state:default`. |
| **`fs:default`** | Read-only filesystem operations (`fs:read-text-file`, `fs:read-file`, `fs:read-dir`, `fs:exists`, `fs:stat`). | Does **NOT** grant `fs:write-text-file` or `fs:remove`. |
| **`fs:remove`** | Deleting files within configured `security.scope`. | Denied unless explicitly granted. |
| **`shell:execute`** | Executing allowlisted programs (`allowed_programs`). | `shell:default` does **NOT** act as a wildcard. |
| **`clipboard:read-text` / `write-text`** | Granular clipboard read/write permissions. | `clipboard:default` does **NOT** act as a wildcard. |
| **`window:create:external`** | Creating windows targeting external HTTP/HTTPS origins (without IPC bridge/token). | Denied under `core:default` and `window:default`. |

---

## 3. Subsystem Security Controls

### 1. Filesystem Scopes & Symlink Containment
* **Deny-by-Default:** Empty `security.scope` denies all filesystem operations; empty `$APPDATA`/`$HOME`/`$DESKTOP` variables fail closed.
* **Canonical Resolution & Dangling Symlinks:** `platform_resolve_path` resolves existing and dangling symlinks iteratively (up to 40 hops) and verifies parent directories, blocking symlink write escapes.
* **Native I/O Primitives:** `tauri:fs:read_dir` uses C++ `platform_read_dir` (`std::filesystem::directory_iterator`, eliminating `struct dirent` ABI mismatches); `tauri:fs:write_text_file` uses `platform_fs_write_file` (`O_NOFOLLOW` on POSIX, reparse-point check on Windows); `tauri:fs:read_text_file` performs bounded chunked reads capped at 50 MB.
* **Strict Glob Matching:** Scope pattern bases escape all Lua pattern metacharacters including `*`; single `*` matches one path segment, `**` matches recursively.

### 2. Direct Process Spawning (`shell:execute`) & Native Sidecars
* **Zero Subshell Execution:** Both `shell:execute` (`platform_process_spawn`) and sidecars (`platform_sidecar_spawn`) invoke `posix_spawnp` (POSIX) or `CreateProcessW` (Windows) directly without `/bin/sh` or `cmd.exe`.
* **Environment Sanitization:** Child environments strip `LD_*`, `DYLD_*`, `NODE_OPTIONS`, `PYTHONPATH`, `PYTHONSTARTUP`, `BASH_ENV`, `ENV`, `IFS`, `GIT_SSH_COMMAND`, `JAVA_TOOL_OPTIONS`, and similar injection variables. Sidecar secrets are passed via `envp`/`CREATE_UNICODE_ENVIRONMENT` and never exposed on command lines (`ps`).
* **Argument & Extension Defenses:** Mandatory `allowed_programs` allowlist, NUL byte rejection in all arguments, dangerous flag blocking (`git -c`/`-C`/`--git-dir`/`--work-tree`, `find -exec`/`-okdir`, `ssh -F`/`ProxyCommand`/`LocalCommand`), and Windows wrapper/ADS blocking (`.bat`, `.cmd`, `.ps1`, `.vbs`, `.com`, `.hta`, `.wsf`, trailing dots, `::$DATA`).
* **Fail-Safe `.env` Policy:** Release bundles unconditionally strip and exclude `.env` and `.env.*` across macOS, Linux, and Windows. Production sidecars load only private per-user overrides (`$APPDATA/<app>/.env`, mode `0600`) or explicit non-secret `sidecar.defaults.env` files.

### 3. LuaRocks Supply Chain Integrity
* **Pure FIPS 180-4 SHA-256:** Implemented natively in LuaJIT inside `cli/tauri.lua` (no shell pipelines, no DJB2 fallback, identical across macOS/Linux/Windows).
* **Recursive Manifest Verification:** Walks all nested subdirectories in `rock_manifest.lua` and `rock_manifest.lib` and rejects any symlink inside `.rocks/`.
* **Fail-Closed Lockfile:** `lua-tauri build`, `lua-tauri rocks verify`, and `lua-tauri rocks install --frozen-lockfile` fail closed on missing, corrupted, unpinned, or mismatched `tauri.lock` SHA-256 entries.

### 4. URL Parsing, SSRF & SQLite Scope Controls
* Rejects userinfo (`@`), backslashes, whitespace, control characters, and invalid ports (`<1` or `>65535`).
* Canonical IPv6 parser blocks loopback (`::1`, `0::1`), unspecified (`::`), IPv4-mapped (`::ffff:`), NAT64 (`64:ff9b::`), 6to4 (`2002::`), Teredo, ULA (`fc00::/7`), link-local (`fe80::/10`), site-local, and multicast (both bracketed and unbracketed).
* Blocks abbreviated/hex/octal IPv4 (`127.1`, `0x7f.0.0.1`), RFC 6890 private/reserved ranges, and cloud metadata hostnames (`169.254.169.254`, `metadata.google.internal`, `metadata.goog`).
* Blocks `sqlite:file:...`, `?`, `#`, traversal, and `@` authority confusion in `is_db_allowed`; redacts URI passwords containing `@` in `mask_db_uri`.

### 5. Single-Instance IPC & Shared State
* **POSIX Single-Instance:** User-private `$XDG_RUNTIME_DIR` or `~/.local/run` (`0700`, verified owner via `lstat`), `0600` socket permissions, fail-closed `getpeereid`/`SO_PEERCRED`, `SO_RCVTIMEO`, 64 KB cap, NUL rejection, and `poll()` + `join()` shutdown.
* **Windows Single-Instance:** Per-user SID in pipe name, owner-only DACL (`D:(A;;GA;;;OW)`), `FILE_FLAG_FIRST_PIPE_INSTANCE`, `PIPE_REJECT_REMOTE_CLIENTS`, and 64 KB / NUL validation.
* **Cross-Worker CAS State:** `State:update` uses C++ atomic version counters (`platform_state_get_version`) with typed booleans and `with_state_lock` exception safety.

### 6. Application Updater
* Intentionally **disabled** (`Not Implemented`). Calls to `tauri:updater:check` and `tauri:updater:install` fail closed.

---

## 4. Platform Verification Matrix

| Subsystem / Control | macOS (WebKit) | Linux (WebKitGTK) | Windows (WebView2) |
| :--- | :---: | :---: | :---: |
| **Origin Gating & Navigation** | Implemented | Implemented | Implemented |
| **IPC Session Token & Unified JSON** | Implemented | Implemented | Implemented |
| **HTTP CSP + `nosniff` + `<meta>` CSP** | HTTP + `<meta>` | HTTP (4.1+) + `<meta>` | `<meta>` + `DENY_CORS` |
| **Frame Isolation** | `isMainFrame` | CSP (`frame-src 'none'`) | `is_frame_source_allowed` + `FrameNavigationStarting` |
| **Filesystem Sandbox & `platform_read_dir`** | Implemented | Implemented | Implemented |
| **Shell-Free Sidecars & `shell:execute`** | Implemented | Implemented | Implemented |
| **Native Menu Bar** | Implemented | Implemented | Implemented |
| **System Tray** | Implemented | Not Implemented | Implemented |
| **Global Shortcuts** | Stub | Stub | Implemented (Win32) |
| **Deep Link Registration** | Info.plist | Desktop Entry | Implemented (HKCU Registry) |
| **Clipboard Text / HTML / Image** | Text, HTML, Image | Text | Text, HTML |
| **CI Matrix & ASan/UBSan** | GitHub Actions | GitHub Actions + ASan/UBSan | GitHub Actions |

---

## 5. Reporting a Vulnerability

Please report security vulnerabilities responsibly via GitHub Private Security Advisories rather than public issues:
* Include attack vector, minimal reproduction, affected platforms, and build flags.
* Reports are acknowledged within 48 hours, with coordinated fixes released prior to public disclosure.
