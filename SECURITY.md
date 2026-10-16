# Security Policy & Threat Model

Tauri-Lua implements a multi-layered **Defense-in-Depth** security architecture inspired by Tauri v2. This document describes the threat model, security boundaries, known architectural limitations, and the vulnerability disclosure process.

---

## 1. Threat Model & Security Boundaries

### Authoritative Security Boundary: Native Origin Gating
The primary and authoritative security boundary in Tauri-Lua is enforced at the native C++ level on every incoming IPC message (`get_native_window_origin` & `OriginValidator::is_allowed`):
* Requests originating from unapproved origins or external domains are unconditionally rejected before reaching Lua handlers.
* Content Security Policy (CSP) headers and DOM meta tags enforce script origin isolation inside the webview.

### Session Token Scope (`window.__TAURI_TOKEN__`)
* A random 32-character session token is attached to `window.__TAURI_TOKEN__` upon webview initialization.
* **Scope & Limitation:** This token acts as a defense-in-depth protection against unauthorized child `<iframe>` elements and cross-origin subframes attempting to access window APIs.
* **Important:** The session token is **not** a same-origin boundary. Any malicious JavaScript executing within the same top-level document context (for example, through an XSS vulnerability) can read `window.__TAURI_TOKEN__`. Same-origin isolation must always be protected via strict Content Security Policy (`script-src 'self'`).

---

## 2. Capabilities & Principle of Least Privilege

Tauri-Lua adheres to a strict **Deny-by-Default** capability model:
1. **Frontend IPC Restriction:** If `security.capabilities` is empty or omitted in `tauri.conf.json`, all frontend IPC calls to host APIs are rejected.
2. **Backend Code vs Frontend Code:** Backend code in `main.lua` runs with full native privileges. Frontend JavaScript code running in the webview is untrusted and restricted by granted capabilities.

### Known Risks & Limitations

#### 1. Frontend SQL Capability (`sql:execute`)
* If the `sql:execute` or `sql:default` capability is granted to the frontend, frontend JavaScript can execute SQL queries against allowed database connections.
* **Defense:** Always enforce connection scopes (`security.sql_scope`) restricting database paths to approved AppData locations, and bind user values with parameterized placeholders (`?` or `$n`) rather than concatenating user input.

#### 2. Shell Execution (`shell:execute`)
* The shell plugin enforces a mandatory `allowed_programs` allowlist. An empty or missing allowlist rejects all execution attempts.
* **Defense:** Restrict `allowed_programs` to specific binary paths, disallow shell wrappers (`.bat`, `.cmd`), and configure per-program argument whitelists to block dangerous parameter injection flags (e.g. `git -c`, `find -exec`).

#### 3. Filesystem Scopes (`security.scope`)
* Filesystem APIs (`fs:read-text-file`, `fs:write-text-file`, `fs:read-dir`) validate canonical paths against declared scopes (`$APPDATA/*`, `$DESKTOP/*`, `$HOME/*`). Directory traversal patterns (`../`, `/./`, `%2e`) are normalized and blocked prior to filesystem access.

---

## 3. Reporting a Vulnerability

If you discover a security vulnerability in Tauri-Lua or official plugins, please report it privately:
* **Email:** security@tauri-lua.dev (or open a private security advisory on GitHub).
* **Information to include:**
  - Description of the vulnerability and attack vector.
  - Minimal reproducible example or proof-of-concept (PoC).
  - Impact assessment (remote code execution, local privilege escalation, sandbox bypass).
* **Response Timeline:** We acknowledge reports within 48 hours and work on remediation prior to public disclosure.
