# Contributing to Tauri-Lua

Thank you for your interest in contributing to **Tauri-Lua**! We welcome bug reports, security audits, documentation improvements, and pull requests from the international community.

## Prerequisites

- **C++17 compiler** (Clang on macOS, GCC/Clang on Linux, MSVC on Windows)
- **CMake** >= 3.16
- **LuaJIT** 2.1
- **Platform SDKs:**
  - macOS: Xcode Command Line Tools (`WebKit`, `Cocoa`, `UserNotifications`)
  - Linux: `libgtk-3-dev` and `libwebkit2gtk-4.1-dev` (or `4.0`)
  - Windows: WebView2 SDK (`deps/webview2/` or `WEBVIEW2_PATH`)

## Building & Testing Locally

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

To verify the fused production build (compile-time release lock):

```bash
cmake -B build-fused -DCMAKE_BUILD_TYPE=Release -DTAURILUA_RELEASE_FUSE=ON
cmake --build build-fused -j
ctest --test-dir build-fused --output-on-failure
```

To run the C++ security tests under AddressSanitizer & UndefinedBehaviorSanitizer:

```bash
cmake -B build-san -DCMAKE_BUILD_TYPE=Debug -DTAURILUA_SANITIZERS=ON
cmake --build build-san --target test_native_security test_origin_validator test_token_standalone
ctest --test-dir build-san --output-on-failure -R "test_native_security|test_origin_validator|test_token_standalone"
```

## Security Standards

All contributions touching the native bridge (`src/`), Lua runtime (`src/tauri/`), or CLI/bundler (`cli/`) must uphold:

1. **Deny-by-default** for capabilities, filesystem scopes, shell executables, and IPC origins.
2. **No subshell execution** for runtime features (use `posix_spawn` / `CreateProcessW` directly).
3. **Zero `.env` secret leakage** into release bundles.
4. **Unified strict RFC 8259 JSON parsing** (`src/json_parser.hpp`) in C++ and depth-bounded encoding/decoding in Lua.
5. **Regression tests** covering every bug fix or security hardening change.

For vulnerability disclosures, follow [SECURITY.md](SECURITY.md) instead of opening a public issue.

## Pull Request Checklist

- [ ] Code compiles cleanly with zero new warnings on both `TAURILUA_RELEASE_FUSE=OFF` and `ON`.
- [ ] All CTest suites (`ctest --test-dir build --output-on-failure`) pass.
- [ ] New behavior or fixes include automated tests in `tests/`.
- [ ] Documentation (`README.md`, `SECURITY.md`, `types/tauri-lua.d.ts`) is updated if APIs or security controls changed.
