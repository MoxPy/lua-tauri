# Security Policy & Threat Model

> [!NOTE]
> **Experimental Architecture Notice**
> Tauri-Lua is an independent, research-oriented desktop runtime combining LuaJIT with a minimal C++17 native bridge and OS-native webviews. This document describes the implemented threat model, defense-in-depth boundaries, platform verification status, and responsible disclosure procedures.

---

## 1. Threat Model & Security Boundaries

### Authoritative Boundary: Native Origin & Navigation Gating
The primary and authoritative security boundary in Tauri-Lua is enforced at the native C++ level on every incoming navigation and IPC message (`SecurityPolicy::is_navigation_allowed` and `OriginValidator::is_allowed`):
* **Release Mode:** Only canonical application schemes (`tauri://localhost`, `https://tauri.localhost`) are permitted. All remote web origins, arbitrary localhost ports, and userinfo-spoofed origins (e.g. `http://localhost:80@attacker.com`) are unconditionally rejected.
* **Dev Mode (`TAURILUA_DEV=1`):** Navigation is permitted to the configured development server (e.g. `http://localhost:1420` or `http://127.0.0.1:1420`). Port and host matching is strictly validated.
* **External URLs:** Navigations to external web origins are intercepted and blocked from the webview context. Any external URL opening must be explicitly delegated via host capabilities to the user's default operating system browser.

### Session Token Scope (`window.__TAURI_TOKEN__`)
* A cryptographically secure 256-bit session token (64 hex characters) is generated on startup via the host operating system CSPRNG:
  - **macOS / BSD:** `arc4random_buf(buf, sizeof(buf))`
  - **Windows:** `BCryptGenRandom(NULL, buf, sizeof(buf), BCRYPT_USE_SYSTEM_PREFERRED_RNG)`
  - **Linux / POSIX:** `getrandom(buf, sizeof(buf), 0)` with fallback to `/dev/urandom`
  - *Fail-Closed:* If the operating system RNG fails or cannot provide entropy, the process immediately logs a critical error and terminates (`std::abort()`).
* **Enforcement:** The session token is verified on every native IPC envelope using constant-time string comparison (`constant_time_eq`), protecting against timing side-channel attacks.
* **Scope & Limitation:** This token acts as a defense-in-depth barrier against unauthorized child `<iframe>` elements and cross-origin subframes attempting to access native host APIs.
* **Important:** The session token is **not** a same-origin sandbox boundary. Any script executing within the top-level document context (such as via an XSS flaw in frontend dependencies) can access `window.__TAURI_TOKEN__`. Same-origin isolation must always be protected with strict Content Security Policy headers (`script-src 'self'`).

### Frame Isolation & Webview Introspection
Frame isolation is enforced via a combination of native webview engine hooks and declarative Content Security Policy:
* **macOS (WebKit):** The native IPC bridge inspects `message.frameInfo.isMainFrame`. IPC messages originating from child iframes or nested browsing contexts are dropped at the native level, even if they share the application's origin.
* **Windows (WebView2):** Incoming messages are gated via `SecurityPolicy::is_frame_source_allowed(source_u8, top_u8, ...)`. If a message originates from a subframe or unapproved frame source, it is rejected before payload extraction.
* **Linux (WebKitGTK):** Navigation actions are evaluated through WebKit navigation policy decisions. Subframe framing attacks are mitigated by default CSP (`frame-src 'none'`, `frame-ancestors 'none'`).

---

## 2. Capabilities & Principle of Least Privilege

Tauri-Lua adheres to a **Deny-by-Default** capability model:
1. **Frontend IPC Restriction:** If `security.capabilities` is empty or omitted in `tauri.conf.json`, all frontend IPC invocations to host APIs fail closed with an explicit unauthorized capability error.
2. **Backend vs Frontend Privilege Separation:** Backend code in `main.lua` runs with full LuaJIT/native privileges on the host. Frontend JavaScript running inside the webview is treated as untrusted and constrained by granted capabilities.

### Capability Presets & Semantics

| Capability | Scope & Permissions Granted | Explicitly NOT Granted |
| :--- | :--- | :--- |
| **`core:default`** | Basic window manipulation (`window:show`, `window:hide`, `window:close`, `window:minimize`, `window:maximize`, `window:center`, `window:set_title`), theme querying, standard event emission/listening, and key-value state access. | Does **NOT** grant `window:create:external`, `updater:*`, `fs:*`, or `shell:*`. |
| **`fs:default`** | Read-only filesystem operations: `fs:read-text-file`, `fs:read-file`, `fs:read-dir`, `fs:exists`, `fs:stat`. | Does **NOT** grant `fs:write-text-file`, `fs:write-file`, or `fs:remove`. |
| **`fs:remove`** | Explicit capability required to delete files or directories (`tauri:fs:remove`). | Cannot be exercised without matching filesystem scope. |
| **`window:create`** | Creating new webview windows pointing to internal application routes (`tauri://localhost/*` or dev URL). | Does **NOT** grant loading external HTTP/HTTPS domains. |
| **`window:create:external`** | Creating windows pointing to arbitrary external web URLs. | Requires explicit security review and deliberate opt-in. |

---

## 3. Subsystem Security Controls

### 1. Filesystem Scopes & Symlink Containment
* **Deny-by-Default:** If `security.scope` is empty or omitted, all filesystem operations are rejected.
* **Path Resolution & Canonicalization:** Both native C++ asset serving (`resolve_contained_asset`, `real_filesystem_path`) and Lua runtime operations (`platform_resolve_path`) canonicalize paths before evaluation.
* **Symlink & Traversal Containment:** Symbolic links and relative directory traversal components (`../`, `/./`, `%2e%2e`) are dereferenced and verified against the canonical scope root. A target path is allowed only if it is strictly prefixed by the allowed directory path with a directory separator boundary (`/`), preventing sibling directory escapes (e.g., `/safe-dir` vs `/safe-dir-secret/`).
* **Path Sanitization:** Paths containing embedded NUL bytes (`\0`) or malformed path separators are rejected immediately.
* **Glob Semantics:**
  - Single asterisk (`*`) matches characters within a single directory segment.
  - Double asterisk (`**`) matches recursively across nested subdirectories.

### 2. Process Spawning (`shell:execute`)
* **Direct Process Spawning:** Commands executed via `shell:execute` are launched directly using native operating system process APIs (`posix_spawnp` on POSIX, `CreateProcessW` on Windows) rather than an intermediate shell (`/bin/sh` or `cmd.exe`).
* **Mandatory Program Allowlist:** The shell engine enforces a strict `allowed_programs` list. If missing or empty, all execution attempts are blocked.
* **Per-Program Argument Gating:**
  - Exact string configurations (e.g. `"git"`) permit execution only with empty argument lists.
  - Configured argument allowlists (e.g. `{ name = "git", args = { "status", "version" } }`) permit only explicit whitelisted parameters.
  - Blanket argument acceptance requires explicit `any_args = true` opt-in.
* **Command Injection Protections (CWE-78):**
  - Direct execution of `.bat` and `.cmd` script wrappers on Windows is blocked to prevent command interpreter argument injection.
  - Dangerous program-specific injection parameters (e.g. `git -c`, `find -exec`) are detected and blocked even when `any_args = true` is configured.
* **Bounded Output Streams:** Output capture is capped at 1 MB per command execution to prevent heap exhaustion or denial-of-service.

### 3. Application Updater
* The application updater is **disabled for security reasons**.
* Unauthenticated archive download and extraction has been completely removed from the runtime core. Calls to `tauri:updater:check` and `tauri:updater:install` fail closed with an explicit error.

### 4. Development Mode vs Release Isolation (`TAURILUA_DEV`)
* **Release Mode:** The runtime does not resolve or load loose files from the current working directory (`CWD`) or bundle `Resources`. Scripts and configuration must be embedded into the binary as pre-compiled LuaJIT bytecode and compiled headers (`tauri_bc.h`, `main_bc.h`, `config_json.h`). `ResourceResolver::resolve_config_json(false, ...)` ignores any loose `tauri.conf.json` on disk to prevent unauthorized capability escalation.
* **Release Packaging:** `lua-tauri build` strictly excludes plain-text Lua source files (`src-tauri/src/*`) and `tauri.conf.json` from the output application bundle.
* **macOS Hardened Runtime & Code Signing:**
  - `src-tauri/entitlements.plist` strictly enables only `com.apple.security.cs.allow-jit` (required for LuaJIT JIT compiler pages) and `com.apple.security.network.client`. Insecure entitlements (`com.apple.security.cs.allow-unsigned-executable-memory` and `com.apple.security.cs.disable-library-validation`) are removed.
  - Release packaging signs all bundled dynamic libraries (`.dylib`, `.so`) individually before signing the application bundle with `--options runtime` and `--entitlements src-tauri/entitlements.plist`, avoiding deprecated `--deep`.
* **Development Mode (`TAURILUA_DEV=1`):** Resolves local workspace scripts and assets for hot-reloading, allows connection to local development HTTP/WebSocket servers, and relaxes CSP `script-src` to include `'unsafe-inline'`.

---

## 4. Platform Verification Matrix

| Subsystem / Control | macOS (WebKit / Cocoa) | Linux (WebKitGTK / GTK3) | Windows (WebView2 / Win32) |
| :--- | :---: | :---: | :---: |
| **Origin Gating & Navigation** | Verified (Native E2E) | Verified (Native Seams) | Verified (Native Seams) |
| **IPC Session Token & Envelope** | Verified (Native E2E) | Verified (Native Seams) | Verified (Native Seams) |
| **CSPRNG Implementation** | `arc4random_buf` | `getrandom` / `/dev/urandom` | `BCryptGenRandom` |
| **Frame Isolation** | `frameInfo.isMainFrame` | CSP (`frame-src 'none'`) | `is_frame_source_allowed` |
| **Symlink Containment** | Verified (Unit + E2E) | Verified (Unit + Seams) | Verified (Unit + Seams) |
| **Shell Argument Validation** | Verified (Automated Suite) | Verified (Automated Suite) | Verified (Automated Suite) |
| **RFC 8259 JSON Parser** | Verified (Automated Suite) | Verified (Automated Suite) | Verified (Automated Suite) |
| **GUI Event Loop Verification** | Verified (macOS Sonoma+) | Requires Linux Display | Requires Windows Desktop |

*Note on Seam Testing:* Platform-specific logic (Windows UTF-16 conversions, WebView2 JSON message extraction, Linux/Windows origin and frame source decision algorithms) is extracted into isolated headers (`src/ipc_envelope.hpp`, `src/origin_check.hpp`, `src/security_policy.hpp`, `src/asset_resolver.hpp`) and verified across unit test suites running under CTest. Full end-to-end GUI validation on Windows and Linux requires respective host display environments.

---

## 5. Reporting a Vulnerability

If you discover a security vulnerability in Tauri-Lua, please report it responsibly:
* Open a private security advisory on GitHub or contact the maintainers directly.
* **Include in your report:**
  - Detailed description of the vulnerability and attack vector.
  - Minimal reproducible demonstration or proof-of-concept.
  - Affected platforms and configurations.
* **Timeline:** Security reports are acknowledged within 48 hours, with patches prepared and reviewed prior to public advisory disclosure.
