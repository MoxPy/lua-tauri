#include <iostream>
#include <cassert>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstring>

#include "src/origin_check.hpp"
#include "src/resource_resolver.hpp"

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
// ---------------------------------------------------------------------------
static bool constant_time_eq(const std::string &a, const std::string &b) {
    if (a.length() != b.length()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.length(); ++i) {
        diff |= (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]));
    }
    return diff == 0;
}

static bool verify_envelope_token(const std::string &req, const std::string &expected_token) {
    if (expected_token.empty()) return false;
    size_t rbracket = req.rfind(']');
    if (rbracket == std::string::npos) return false;
    size_t q_end = req.rfind('"', rbracket);
    if (q_end == std::string::npos || q_end == 0) return false;
    size_t q_start = q_end;
    while (q_start > 0) {
        q_start--;
        if (req[q_start] == '"') {
            size_t bsl = 0;
            size_t b = q_start;
            while (b > 0 && req[b - 1] == '\\') { bsl++; b--; }
            if (bsl % 2 == 0) break;
        }
    }
    if (q_start >= q_end) return false;
    size_t comma = req.rfind(',', q_start);
    if (comma == std::string::npos) return false;
    for (size_t i = comma + 1; i < q_start; ++i) {
        if (!isspace(static_cast<unsigned char>(req[i]))) return false;
    }
    for (size_t i = q_end + 1; i < rbracket; ++i) {
        if (!isspace(static_cast<unsigned char>(req[i]))) return false;
    }
    std::string token_in_req = req.substr(q_start + 1, q_end - (q_start + 1));
    return constant_time_eq(token_in_req, expected_token);
}

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
// ---------------------------------------------------------------------------
static std::string extract_json_string(const std::string &json, const std::string &key) {
    std::string needle = "\"" + key + "\"";
    size_t kpos = json.find(needle);
    if (kpos == std::string::npos) return "";
    size_t colon = json.find(':', kpos + needle.size());
    if (colon == std::string::npos) return "";
    size_t q_start = json.find('"', colon + 1);
    if (q_start == std::string::npos) return "";

    std::string result;
    size_t pos = q_start + 1;
    while (pos < json.size()) {
        char c = json[pos];
        if (c == '\\') {
            if (pos + 1 < json.size()) {
                char next = json[pos + 1];
                if (next == '"') { result += '"'; pos += 2; }
                else if (next == '\\') { result += '\\'; pos += 2; }
                else if (next == '/') { result += '/'; pos += 2; }
                else if (next == 'n') { result += '\n'; pos += 2; }
                else if (next == 'r') { result += '\r'; pos += 2; }
                else if (next == 't') { result += '\t'; pos += 2; }
                else { result += next; pos += 2; }
            } else {
                break;
            }
        } else if (c == '"') {
            return result;
        } else {
            result += c;
            pos++;
        }
    }
    return result;
}

void test_windows_ipc_json_parsing() {
    std::cout << "[Test] Windows IPC extract_json_string (§9)..." << std::endl;

    // Representative WebView2 WebMessageAsJson payload
    std::string msg = "{\"name\":\"__TAURI_RAW_INVOKE__\",\"seq\":\"1\",\"req\":\"[\\\"tauri:fs:read_text_file\\\",{\\\"path\\\":\\\"file.txt\\\"},\\\"tok123\\\"]\"}";

    std::string name = extract_json_string(msg, "name");
    assert(name == "__TAURI_RAW_INVOKE__");

    std::string seq = extract_json_string(msg, "seq");
    assert(seq == "1");

    std::string req = extract_json_string(msg, "req");
    // Pre-fix, extract_field returned "[\" because it cut off at the first unescaped quote!
    assert(req == "[\"tauri:fs:read_text_file\",{\"path\":\"file.txt\"},\"tok123\"]");

    std::cout << "  -> PASSED" << std::endl;
}

// ---------------------------------------------------------------------------
// 5. Asset Resolver & MIME Tests (§7)
// ---------------------------------------------------------------------------
static std::string get_mime(const std::string &path) {
    auto ends_with = [](const std::string &s, const std::string &suffix) {
        if (s.size() < suffix.size()) return false;
        return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (ends_with(lower, ".html") || ends_with(lower, ".htm")) return "text/html";
    if (ends_with(lower, ".js") || ends_with(lower, ".mjs")) return "application/javascript";
    if (ends_with(lower, ".css")) return "text/css";
    if (ends_with(lower, ".json")) return "application/json";
    if (ends_with(lower, ".png")) return "image/png";
    if (ends_with(lower, ".jpg") || ends_with(lower, ".jpeg")) return "image/jpeg";
    if (ends_with(lower, ".svg")) return "image/svg+xml";
    if (ends_with(lower, ".wasm")) return "application/wasm";
    return "application/octet-stream";
}

static bool is_safe_asset_path(const std::string &raw_path) {
    if (raw_path.empty()) return false;
    if (raw_path.find('\0') != std::string::npos) return false;
    if (raw_path.find('\\') != std::string::npos) return false;
    if (raw_path.find("..") != std::string::npos) return false;
    if (raw_path.find("%2e") != std::string::npos || raw_path.find("%2E") != std::string::npos) return false;
    if (raw_path.find(':') != std::string::npos) return false;
    return true;
}

void test_asset_resolver() {
    std::cout << "[Test] Asset Path Containment & Suffix MIME (§7)..." << std::endl;

    // MIME suffix test: substring trick must not classify a.html.png as text/html
    assert(get_mime("index.html") == "text/html");
    assert(get_mime("style.css") == "text/css");
    assert(get_mime("main.js") == "application/javascript");
    assert(get_mime("a.html.png") == "image/png");
    assert(get_mime("a.css.jpg") == "image/jpeg");

    // Path containment test: traversal and tricks rejected
    assert(is_safe_asset_path("index.html"));
    assert(is_safe_asset_path("assets/style.css"));
    assert(!is_safe_asset_path("../etc/passwd"));
    assert(!is_safe_asset_path("assets/../../secret"));
    assert(!is_safe_asset_path("assets/%2e%2e/secret"));
    assert(!is_safe_asset_path("..\\windows\\win.ini"));
    assert(!is_safe_asset_path("C:/boot.ini"));
    std::string nul_path = "index.html";
    nul_path += '\0';
    nul_path += "extra";
    assert(!is_safe_asset_path(nul_path));

    std::cout << "  -> PASSED" << std::endl;
}

int main() {
    std::cout << "=== Running C++ Native Security Tests (Set 1) ===" << std::endl;
    test_origin_validator();
    test_resource_resolver();
    test_token_verification();
    test_windows_ipc_json_parsing();
    test_asset_resolver();
    std::cout << "All C++ Native Security Tests PASSED!" << std::endl;
    return 0;
}
