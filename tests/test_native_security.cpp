#include <iostream>
#include <cassert>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <filesystem>

#include "src/origin_check.hpp"
#include "src/resource_resolver.hpp"
#include "src/asset_resolver.hpp"
#include "src/security_policy.hpp"
#include "src/ipc_envelope.hpp"

// ---------------------------------------------------------------------------
// 1. OriginValidator Tests (§4)
// ---------------------------------------------------------------------------
void test_origin_validator() {
    std::cout << "[Test] OriginValidator (§4)..." << std::endl;

    // Release mode valid app origins
    assert(OriginValidator::is_allowed("tauri://localhost", false));
    assert(OriginValidator::is_allowed("tauri://localhost/", false));
    assert(OriginValidator::is_allowed("tauri://localhost/index.html", false));
    assert(OriginValidator::is_allowed("https://tauri.localhost", false));
    assert(OriginValidator::is_allowed("https://tauri.localhost/", false));
    assert(OriginValidator::is_allowed("https://tauri.localhost/bundle.js", false));

    // Release mode MUST deny arbitrary localhost ports and remote origins
    assert(!OriginValidator::is_allowed("http://localhost:31337/evil.html", false));
    assert(!OriginValidator::is_allowed("http://127.0.0.1:8080/", false));
    assert(!OriginValidator::is_allowed("http://localhost:1420/", false));
    assert(!OriginValidator::is_allowed("https://example.com/", false));

    // Userinfo bypass attacks (@) MUST BE STRICTLY DENIED in both release and dev
    assert(!OriginValidator::is_allowed("http://localhost:80@evil.example/pwn.html", false));
    assert(!OriginValidator::is_allowed("http://localhost:80@evil.example/pwn.html", true, "http://localhost:80"));
    assert(!OriginValidator::is_allowed("http://127.0.0.1:x@attacker.example/", true, "http://127.0.0.1:80"));
    assert(!OriginValidator::is_allowed("https://tauri.localhost:x@attacker.example/", false));
    assert(!OriginValidator::is_allowed("tauri://localhost:x@evil.example/", false));

    // Hostname lookalikes MUST BE DENIED
    assert(!OriginValidator::is_allowed("http://localhost.evil.example/", true, "http://localhost:1420"));
    assert(!OriginValidator::is_allowed("http://localhostevil.example/", true, "http://localhost:1420"));

    // Case insensitivity & port matching in dev mode
    assert(OriginValidator::is_allowed("HTTP://LOCALHOST:1420/", true, "http://localhost:1420"));
    assert(OriginValidator::is_allowed("http://localhost:1420/test", true, "http://localhost:1420"));
    assert(OriginValidator::is_allowed("http://127.0.0.1:1420/", true, "http://localhost:1420"));
    assert(!OriginValidator::is_allowed("http://localhost:3000/", true, "http://localhost:1420")); // mismatched port

    // Empty or malformed inputs
    assert(!OriginValidator::is_allowed("", false));
    assert(!OriginValidator::is_allowed("", true));
    assert(!OriginValidator::is_allowed("invalid-scheme://localhost", false));

    std::cout << "  -> PASSED" << std::endl;
}

// ---------------------------------------------------------------------------
// 2. ResourceResolver Tests (§1, §5)
// ---------------------------------------------------------------------------
void test_resource_resolver() {
    std::cout << "[Test] ResourceResolver (§1, §5)..." << std::endl;

    // In release mode (dev_mode = false), never resolve loose CWD main.lua or tauri.conf.json
    std::string release_main = ResourceResolver::resolve_main_lua(false);
    assert(release_main.empty()); // Release mode does not load loose CWD scripts!

    // In release mode, resolve_config_json returns embedded content
    const char dummy_embedded[] = "{\"embedded\": true}";
    std::string release_cfg = ResourceResolver::resolve_config_json(false, (const unsigned char *)dummy_embedded, sizeof(dummy_embedded) - 1);
    assert(release_cfg == dummy_embedded);

    std::string release_dist = ResourceResolver::resolve_assets_root(false, "dist");
    assert(!release_dist.empty());

    // In dev mode (dev_mode = true), loose development files can be resolved
    std::string dev_cfg = ResourceResolver::resolve_config_json(true, (const unsigned char *)dummy_embedded, sizeof(dummy_embedded) - 1);
    assert(!dev_cfg.empty());

    std::cout << "  -> PASSED" << std::endl;
}

// ---------------------------------------------------------------------------
// 3. Token Envelope & Constant-Time Verification Tests (§9)
// Tested directly against production functions in src/ipc_envelope.hpp
// ---------------------------------------------------------------------------
void test_token_verification() {
    std::cout << "[Test] Token Verification & Envelope Extraction (§9)..." << std::endl;

    std::string secret = "4f8a3c2b1e0d9f8e7d6c5b4a3f2e1d0c4f8a3c2b1e0d9f8e7d6c5b4a3f2e1d0c";

    // Valid envelope
    std::string valid = "[\"tauri:fs:read_text_file\",{\"path\":\"test.txt\"},\"" + secret + "\"]";
    assert(verify_envelope_token(valid, secret));

    // Valid envelope with whitespace
    std::string valid_ws = "[\"cmd\" , {\"a\":1} , \"" + secret + "\" ]";
    assert(verify_envelope_token(valid_ws, secret));

    // Token placed in payload rather than envelope 3rd position MUST BE REJECTED
    std::string token_in_payload = "[\"cmd\",{\"token\":\"" + secret + "\"},\"wrong_token\"]";
    assert(!verify_envelope_token(token_in_payload, secret));

    // 2-element envelope without token MUST BE REJECTED
    std::string missing_token = "[\"cmd\",{\"token\":\"" + secret + "\"}]";
    assert(!verify_envelope_token(missing_token, secret));

    // Escaped quotes in payload should not confuse token finder
    std::string escaped_quotes = "[\"cmd\",{\"data\":\"quote \\\" inside\"},\"" + secret + "\"]";
    assert(verify_envelope_token(escaped_quotes, secret));

    std::cout << "  -> PASSED" << std::endl;
}

// ---------------------------------------------------------------------------
// 4. Windows extract_json_string Tests (§9)
// Tested directly against production functions in src/ipc_envelope.hpp
// ---------------------------------------------------------------------------

void test_windows_ipc_json_parsing() {
    std::cout << "[Test] Windows IPC extract_json_string (§9)..." << std::endl;

    // Representative WebView2 WebMessageAsJson payload
    std::string msg = "{\"name\":\"__TAURI_RAW_INVOKE__\",\"seq\":\"1\",\"req\":\"[\\\"tauri:fs:read_text_file\\\",{\\\"path\\\":\\\"file.txt\\\"},\\\"tok123\\\"]\"}";

    std::string name = extract_json_string(msg, "name");
    assert(name == "__TAURI_RAW_INVOKE__");

    std::string seq = extract_json_string(msg, "seq");
    assert(seq == "1");

    std::string req = extract_json_string(msg, "req");
    assert(req == "[\"tauri:fs:read_text_file\",{\"path\":\"file.txt\"},\"tok123\"]");

    std::cout << "  -> PASSED" << std::endl;
}

// ---------------------------------------------------------------------------
// 5. Production Asset Resolver & Symlink Containment Tests (§7)
// ---------------------------------------------------------------------------
void test_asset_resolver() {
    std::cout << "[Test] Production Asset Resolver & Symlink Containment (resolve_contained_asset)..." << std::endl;

    // 1. MIME suffix test from asset_resolver.hpp
    assert(get_mime_type("index.html") == "text/html");
    assert(get_mime_type("style.css") == "text/css");
    assert(get_mime_type("main.js") == "application/javascript");
    assert(get_mime_type("a.html.png") == "image/png");
    assert(get_mime_type("a.css.jpg") == "image/jpeg");
    assert(get_mime_type("image.ico") == "image/x-icon");
    assert(get_mime_type("app.wasm") == "application/wasm");
    assert(get_mime_type("unknown.xyz") == "application/octet-stream");

    // 2. Real filesystem containment test
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / ("tauri_test_assets_" + std::to_string(rand()));
    std::filesystem::path outside_dir = std::filesystem::temp_directory_path() / ("tauri_test_outside_" + std::to_string(rand()));
    std::error_code ec;
    std::filesystem::create_directories(temp_dir / "sub", ec);
    std::filesystem::create_directories(outside_dir, ec);

    // Create real files in asset tree
    std::ofstream(temp_dir / "index.html") << "<h1>Test</h1>";
    std::ofstream(temp_dir / "style.css") << "body { color: red; }";
    std::ofstream(temp_dir / "sub" / "app.js") << "console.log('ok');";

    // Create a secret file outside the asset tree
    std::filesystem::path secret_file = outside_dir / "secret.txt";
    std::ofstream(secret_file) << "SUPER_SECRET_KEY";

    // Create symlink inside pointing to internal file (safe)
    std::filesystem::path internal_link = temp_dir / "internal_link.css";
    std::filesystem::create_symlink(temp_dir / "style.css", internal_link, ec);

    // Create symlink pointing outside asset root (dangerous jailbreak attempt!)
    std::filesystem::path escape_link = temp_dir / "escape_link.txt";
    std::filesystem::create_symlink(secret_file, escape_link, ec);

    std::string root_str = temp_dir.string();
    std::string out_path;

    // (a) Valid files resolve correctly
    assert(resolve_contained_asset(root_str, "index.html", out_path));
    assert(std::filesystem::equivalent(out_path, temp_dir / "index.html"));

    assert(resolve_contained_asset(root_str, "/", out_path)); // Root resolves to index.html
    assert(std::filesystem::equivalent(out_path, temp_dir / "index.html"));

    assert(resolve_contained_asset(root_str, "sub/app.js", out_path));
    assert(std::filesystem::equivalent(out_path, temp_dir / "sub" / "app.js"));

    // (b) Internal symlink resolves safely
    if (std::filesystem::exists(internal_link)) {
        assert(resolve_contained_asset(root_str, "internal_link.css", out_path));
        assert(std::filesystem::equivalent(out_path, temp_dir / "style.css"));
    }

    // (c) External symlink escaping root MUST BE REJECTED
    if (std::filesystem::exists(escape_link)) {
        assert(!resolve_contained_asset(root_str, "escape_link.txt", out_path));
    }

    // (d) Directory traversal attacks MUST BE REJECTED
    assert(!resolve_contained_asset(root_str, "../etc/passwd", out_path));
    assert(!resolve_contained_asset(root_str, "sub/../../secret.txt", out_path));
    assert(!resolve_contained_asset(root_str, "sub/%2e%2e/%2e%2e/secret.txt", out_path));
    assert(!resolve_contained_asset(root_str, "sub/%2E%2E/app.js", out_path));
    assert(!resolve_contained_asset(root_str, "..\\windows\\win.ini", out_path));
    assert(!resolve_contained_asset(root_str, "C:/boot.ini", out_path));
    assert(!resolve_contained_asset(root_str, "/etc/passwd", out_path));

    // (e) Injection tricks (NUL bytes, directories, non-existent)
    std::string nul_str = "index.html"; nul_str += '\0'; nul_str += "extra";
    assert(!resolve_contained_asset(root_str, nul_str, out_path));
    assert(!resolve_contained_asset(root_str, "index.html%00extra", out_path));
    assert(!resolve_contained_asset(root_str, "sub", out_path)); // directory rejection
    assert(!resolve_contained_asset(root_str, "non_existent.html", out_path));

    // Cleanup
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::remove_all(outside_dir, ec);

    std::cout << "  -> PASSED" << std::endl;
}

// ---------------------------------------------------------------------------
// 6. JSON String Escaping for File Drops & Interop (Point 2)
// ---------------------------------------------------------------------------
void test_json_escaping() {
    std::cout << "[Test] RFC 8259 JSON String Escaping (json_escape_string)..." << std::endl;

    // Control characters (\n, \r, \t, \b, \f)
    std::string with_ctrl = "line1\nline2\r\ttabbed\b\fform";
    std::string esc_ctrl = json_escape_string(with_ctrl);
    assert(esc_ctrl == "\"line1\\nline2\\r\\ttabbed\\b\\fform\"");

    // Quotes and backslashes
    std::string with_quotes = "path\\to\\\"file\".txt";
    std::string esc_quotes = json_escape_string(with_quotes);
    assert(esc_quotes == "\"path\\\\to\\\\\\\"file\\\".txt\"");

    // Low ASCII control characters (< 0x20)
    std::string low_ascii = std::string("test") + '\x01' + '\x1f';
    std::string esc_low = json_escape_string(low_ascii);
    assert(esc_low == "\"test\\u0001\\u001f\"");

    // UTF-8 multi-byte characters must NOT be corrupted or over-escaped
    std::string unicode_str = "/Users/test/日本語_ディレクトリ/Café_☕_🚀.png";
    std::string esc_unicode = json_escape_string(unicode_str);
    assert(esc_unicode == "\"/Users/test/日本語_ディレクトリ/Café_☕_🚀.png\"");

    // Full JSON array construction mimicking platform_dispatch_file_drop
    std::vector<std::string> paths = {
        "/path/to/normal.txt",
        "/path/with\nnewline/file.txt",
        "/path/with\"quotes\"/file.png",
        "/path/with\\backslash/file.pdf",
        "/path/with_unicode_漢字/Café.jpg"
    };
    std::string json = "{\"paths\":[";
    for (size_t i = 0; i < paths.size(); ++i) {
        if (i > 0) json += ",";
        json += json_escape_string(paths[i]);
    }
    json += "],\"position\":{\"x\":100,\"y\":200}}";

    assert(json.find("\"/path/with\\nnewline/file.txt\"") != std::string::npos);
    assert(json.find("\"/path/with\\\"quotes\\\"/file.png\"") != std::string::npos);
    assert(json.find("\"/path/with\\\\backslash/file.pdf\"") != std::string::npos);
    assert(json.find("\"/path/with_unicode_漢字/Café.jpg\"") != std::string::npos);

    std::cout << "  -> PASSED" << std::endl;
}

// ---------------------------------------------------------------------------
// 7. Robust Dual-Layer CSP Meta Injection (Point 5)
// ---------------------------------------------------------------------------
void test_csp_injection() {
    std::cout << "[Test] Robust CSP Meta Injection (inject_csp_meta)..." << std::endl;

    std::string default_csp = "default-src 'self' tauri: asset:; script-src 'self' tauri: asset:;";

    // (a) Normal HTML with <head>
    std::string html1 = "<!DOCTYPE html><html><head><title>App</title></head><body></body></html>";
    inject_csp_meta(html1, default_csp);
    assert(html1.find("<meta http-equiv=\"Content-Security-Policy\" content=\"" + default_csp + "\">") != std::string::npos);

    // (b) Case-insensitive <HEAD> with attributes
    std::string html2 = "<html><HEAD class=\"theme-dark\"><title>App</title></HEAD></html>";
    inject_csp_meta(html2, default_csp);
    assert(html2.find("<meta http-equiv=\"Content-Security-Policy\" content=\"" + default_csp + "\">") != std::string::npos);

    // (c) Comment containing Content-Security-Policy must NOT prevent injection
    std::string html3 = "<html><!-- Comment mentioning Content-Security-Policy --><head></head></html>";
    inject_csp_meta(html3, default_csp);
    assert(has_existing_csp_meta(html3));
    assert(html3.find("<meta http-equiv=\"Content-Security-Policy\"") != std::string::npos);

    // (d) Script string containing Content-Security-Policy must NOT prevent injection
    std::string html4 = "<html><head><script>console.log('Content-Security-Policy header');</script></head></html>";
    inject_csp_meta(html4, default_csp);
    assert(has_existing_csp_meta(html4));
    assert(html4.find("<meta http-equiv=\"Content-Security-Policy\"") != std::string::npos);

    // (e) Existing real CSP meta tag must NOT be duplicated
    std::string html5 = "<html><head><meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'self'\"></head></html>";
    std::string orig = html5;
    inject_csp_meta(html5, default_csp);
    assert(html5 == orig); // Unchanged!

    // (f) HTML without <head> tag
    std::string html6 = "<!DOCTYPE html><html><body><h1>Hello</h1></body></html>";
    inject_csp_meta(html6, default_csp);
    assert(html6.find("<meta http-equiv=\"Content-Security-Policy\"") != std::string::npos);
    assert(html6.find("<head>") != std::string::npos);

    std::cout << "  -> PASSED" << std::endl;
}

// ---------------------------------------------------------------------------
// 8. Security Policy, Frame Origin & Navigation Tests (N4)
// ---------------------------------------------------------------------------
void test_security_policy() {
    std::cout << "[Test] SecurityPolicy CSP, Navigation & Frame Origin (N4)..." << std::endl;

    // 1. CSP builder checks
    std::string release_csp = SecurityPolicy::build_default_csp(false);
    assert(release_csp.find("frame-src 'none'") != std::string::npos);
    assert(release_csp.find("object-src 'none'") != std::string::npos);
    assert(release_csp.find("base-uri 'self'") != std::string::npos);
    assert(release_csp.find("form-action 'self'") != std::string::npos);
    assert(release_csp.find("frame-ancestors 'none'") != std::string::npos);

    std::string dev_csp = SecurityPolicy::build_default_csp(true);
    assert(dev_csp.find("frame-src 'none'") != std::string::npos);
    assert(dev_csp.find("frame-ancestors 'none'") != std::string::npos);

    std::string custom = "default-src 'none';";
    assert(SecurityPolicy::build_default_csp(false, custom) == custom);

    // 2. Navigation Policy checks
    assert(SecurityPolicy::is_navigation_allowed("tauri://localhost", false));
    assert(SecurityPolicy::is_navigation_allowed("https://tauri.localhost/index.html", false));
    assert(SecurityPolicy::is_navigation_allowed("about:blank", false));
    assert(!SecurityPolicy::is_navigation_allowed("https://tauri.localhost.evil.com/x", false));
    assert(!SecurityPolicy::is_navigation_allowed("https://tauri.localhost@evil.com/", false));
    assert(!SecurityPolicy::is_navigation_allowed("https://evil.com/phish", false));
    assert(!SecurityPolicy::is_navigation_allowed("http://localhost:3000/", false));

    // 3. Subframe IPC / Message source checks
    assert(SecurityPolicy::is_frame_source_allowed("https://tauri.localhost/page.html", "https://tauri.localhost/index.html", false));
    assert(!SecurityPolicy::is_frame_source_allowed("https://evil.com/iframe.html", "https://tauri.localhost/index.html", false));
    assert(!SecurityPolicy::is_frame_source_allowed("https://tauri.localhost@evil.com/", "https://tauri.localhost/index.html", false));

    std::cout << "  -> PASSED" << std::endl;
}

int main() {
    std::cout << "=== Running C++ Native Security Tests (Set 1 & Set 2) ===" << std::endl;
    test_origin_validator();
    test_resource_resolver();
    test_token_verification();
    test_windows_ipc_json_parsing();
    test_asset_resolver();
    test_json_escaping();
    test_csp_injection();
    test_security_policy();
    std::cout << "All C++ Native Security Tests PASSED!" << std::endl;
    return 0;
}
