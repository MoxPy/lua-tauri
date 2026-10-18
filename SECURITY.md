# Security Policy & Threat Model

Tauri-Lua implements a multi-layered **Defense-in-Depth** security architecture inspired by Tauri v2. This document describes the threat model, security boundaries, known architectural limitations, and the vulnerability disclosure process.

---

## 1. Threat Model & Security Boundaries

### Authoritative Security Boundary: Native Origin Gating
The primary and authoritative security boundary in Tauri-Lua is enforced at the native C++ level on every incoming IPC message (`get_native_window_origin` & `OriginValidator::is_allowed`):
* Requests originating from unapproved origins or external domains are unconditionally rejected before reaching Lua handlers.
* Content Security Policy (CSP) headers and DOM meta tags enforce script origin isolation inside the webview.

### Session Token Scope (`window.__TAURI_TOKEN__`)
* A cryptographically secure 256-bit session token (64 hex characters) is generated via the host OS CSPRNG (`SecRandomCopyBytes` on macOS, `/dev/urandom` on Linux, `BCryptGenRandom` on Windows) and injected into `window.__TAURI_TOKEN__` upon webview initialization.
* **Scope & Limitation:** This token acts as a defense-in-depth barrier against unauthorized child `<iframe>` elements and cross-origin subframes attempting to access window APIs.
* **Important:** The session token is **not** a same-origin boundary. Any malicious JavaScript executing within the same top-level document context (for example, through an XSS vulnerability) can read `window.__TAURI_TOKEN__`. Same-origin isolation must always be protected via strict Content Security Policy (`script-src 'self'`).

### Frame Isolation & Webview Introspection
* **macOS (WebKit):** The native IPC bridge inspects `message.frameInfo.isMainFrame`. IPC messages originating from child iframes or nested browsing contexts are rejected at the native level, even if they share the application's origin.
* **Linux (WebKitGTK) & Windows (WebView2):** Where underlying webview engines do not expose frame metadata directly on IPC message callbacks, subframe framing attacks are mitigated at the CSP layer (`frame-src 'none'`), preventing untrusted frames from being loaded.

---

## 2. Capabilities & Principle of Least Privilege

Tauri-Lua adheres to a strict **Deny-by-Default** capability model:
1. **Frontend IPC Restriction:** If `security.capabilities` is empty or omitted in `tauri.conf.json`, all frontend IPC calls to host APIs are rejected.
2. **Backend Code vs Frontend Code:** Backend code in `main.lua` runs with full native privileges. Frontend JavaScript code running in the webview is untrusted and restricted by granted capabilities.

### Subsystem Security Details & Controls

#### 1. Filesystem Scopes (`security.scope`)
* **Deny-by-Default:** If `security.scope` is empty or undefined, all filesystem access from the frontend is denied.
* **Canonical Path Resolution:** Path aliases (`$APPDATA/*`, `$DESKTOP/*`, `$HOME/*`, `$RESOURCE/*`) are resolved canonically. Directory traversal attempts (`../`, `/./`, symlink escapes) are detected and blocked prior to filesystem operations.
* **Glob Wildcard Semantics:**
  - Single asterisk (`*`) matches characters within a single path segment.
  - Double asterisk (`**`) matches recursively across multiple directory levels.
* **Path Sanitization:** Paths containing NUL bytes (`\0`) or invalid characters are rejected immediately.
* **Granular Deletion Control:** File deletion (`tauri:fs:remove`) requires explicit grant of `fs:remove` (or wildcard `fs:*`) and cannot be performed with `fs:write-text-file` alone.

#### 2. Process Spawning (`shell:execute`)
* **Direct Process Spawning:** Commands executed via `shell:execute` are launched directly using native operating system process APIs (`posix_spawnp` on POSIX, `CreateProcessW` on Windows) rather than an intermediate shell (`/bin/sh` or `cmd.exe`).
* **Allowlist Enforcement:** The shell engine enforces a mandatory `allowed_programs` allowlist. An empty or missing allowlist rejects all execution attempts.
* **Argument Whitelisting & Flag Filtering:** Arguments are checked against configured allowlists, and dangerous injection flags (e.g. `git -c`, `find -exec`) are blocked.
* **Bounded Output Streams:** Output capture is capped at 1 MB per execution to prevent heap exhaustion or denial-of-service.

#### 3. Frontend SQL Capability (`sql:execute`)
* If the `sql:execute` or `sql:default` capability is granted to the frontend, frontend JavaScript can execute SQL queries against allowed database connections.
* **Defense:** Always enforce connection scopes (`security.sql_scope`) restricting database paths to approved AppData locations, and bind user values with parameterized placeholders (`?` or `$n`) rather than concatenating user input.

#### 4. Developer Configuration & Build Scripts
* Project configuration (`tauri.conf.json`) and developer lifecycle hooks (`beforeDevCommand`, `beforeBuildCommand`) represent developer-authored code executed on the developer workstation, not untrusted input from the webview.

#### 5. Application Updater
* The application updater is **disabled for security reasons**. Unauthenticated archive download and extraction has been removed from the runtime; updater API calls fail closed with an explicit error.

---

## 3. Reporting a Vulnerability

If you discover a security vulnerability in Tauri-Lua or official plugins, please report it privately:
* Open a private security advisory on GitHub.
* **Information to include:**
  - Description of the vulnerability and attack vector.
  - Minimal reproducible example or proof-of-concept (PoC).
  - Impact assessment (remote code execution, local privilege escalation, sandbox bypass).
* **Response Timeline:** We acknowledge reports within 48 hours and work on remediation prior to public disclosure.
