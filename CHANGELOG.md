# Changelog

All notable changes to **Tauri-Lua** are documented in this file. The format adheres to [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-11-20

### Security
- **Unified C++17 RFC 8259 JSON Parser (`src/json_parser.hpp`):** Replaced all fragmented C++ JSON parsers with a single strict, depth-bounded parser (`max_depth = 64`), eliminating infinite loops, stack overflows, and parser differentials across IPC, menus, and config loading.
- **Compile-Time Release Fuse (`TAURILUA_RELEASE_FUSE=ON`):** Production builds created via `lua-tauri build` lock out `TAURILUA_DEV`, `TAURI_LUA_ROOT`, CWD asset fallback, and relative `ffi.load` paths at compile time.
- **Fail-Safe `.env` Policy:** Release bundles on macOS, Linux, and Windows unconditionally exclude `.env` and `.env.*` files. Runtime sidecars only load private per-user overrides (`$APPDATA/<app>/.env`) or explicit non-secret `sidecar.defaults.env` defaults.
- **Native Notification & WebView Hardening:** Fixed macOS `osascript` option-injection (`--` terminator) and duplicate delivery (`__block bool delivered`); fixed Linux asset Use-After-Free (`g_malloc`/`g_free`); replaced Windows `WM_USER+101` raw pointer dereference with a mutex-protected dispatch queue; enabled Objective-C ARC (`-fobjc-arc`) on macOS; added WebView2 `DENY_CORS`, `add_FrameNavigationStarting`, and WebKitGTK 4.1 HTTP CSP/`nosniff` headers.
- **Filesystem Sandbox & Native Directory APIs:** Added `platform_read_dir` (fixing macOS ARM64 `struct dirent` ABI corruption) and `platform_fs_write_file` (`O_NOFOLLOW` / reparse-point check); resolved dangling symlinks iteratively in `platform_resolve_path`; enforced fail-closed handling on empty `$APPDATA`/`$HOME`/`$DESKTOP` and escaped `*` in scope patterns; bounded `fs:read_text_file` to 50 MB chunked reads.
- **Shell, Capability, SSRF & SQL Hardening:** Fixed `assert_program` with `any_args = true`, blocked NUL bytes and extended dangerous flag/extension denylists, sanitized child `environ`, restricted `shell:default`/`clipboard:default`/`store:default`, added canonical IPv6/abbreviated-IPv4 SSRF blocking, and blocked `sqlite:file:` / `@` authority confusion.
- **Native Shell-Free Sidecars (`platform_sidecar_spawn`):** Eliminated `/bin/sh` and `cmd.exe` wrappers for sidecars across POSIX and Windows, keeping secrets out of `ps`, enforcing `0600` + `O_NOFOLLOW` logs, and removing `kill -9` PID-reuse races.
- **LuaRocks Supply Chain Integrity:** Replaced shell-based/DJB2 checksums with pure LuaJIT FIPS 180-4 SHA-256, recursive subdirectory manifest walking with symlink rejection, fail-closed `tauri.lock` verification, `lua-tauri rocks verify`, and `--frozen-lockfile`.
- **Packaging & Scaffolding Hardening:** Enforced fail-hard macOS `codesign` (including Mach-O sidecars, `--timestamp`, `--verify --deep --strict`, and DMG signing), `--root-owner-group` + `/usr/lib/<pkg>/` on `.deb`, strict app metadata validation, Vite `^6.2.6` in scaffolded templates, restricted default scope to `$APPDATA/<identifier>/**`, and extracted inline scripts from `init` for strict CSP compatibility.

### Fixed
- **Concurrency & Cross-Platform Stability:** Eliminated AB-BA deadlocks across all `platform_window_*` APIs; added GTK main-thread marshaling (`g_main_context_invoke`); moved `State:update` CAS versioning into C++ (`platform_state_get_version`) with typed booleans (`platform_state_set_bool`/`get_bool`); hardened single-instance IPC (POSIX `poll` + `join` + `0700` dir verification; Windows User SID + owner-only DACL + `FILE_FLAG_FIRST_PIPE_INSTANCE`).
- **C Embedding API (`libtaurilua`):** Propagated host scripts, configs, and C commands (`taurilua_register_command`) to all `IsolatePool` workers; applied all 9 `taurilua_config_t` fields; repaired `taurilua_open_window()`; removed brutal `exit(0)` calls on macOS window close; added ABI version macros, `TAURILUA_STATIC`, `taurilua_set_startup_payload`, and `taurilua_last_error`.
- **CLI & Tooling:** Fixed symlink resolution in `./lua-tauri`, Windows directory detection in `file_exists`, React `TS6133`, missing Vue/Svelte `tsconfig.json`, and `Tauri.sleep` >= 1000 ms on POSIX.

### Added
- Multi-OS GitHub Actions CI workflow (macOS, Ubuntu, Windows + ASan/UBSan sanitizer job).
- Official TypeScript declarations (`types/tauri-lua.d.ts`).
- CMake `install()` targets and `TAURILUA_SANITIZERS` option.
- Community governance files (`CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`, `CHANGELOG.md`, `.clang-format`, GitHub issue templates).
