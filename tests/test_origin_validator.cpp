#include "src/origin_check.hpp"
#include <iostream>
#include <cassert>

int main() {
    // Release mode:
    assert(OriginValidator::is_allowed("tauri://localhost", false));
    assert(OriginValidator::is_allowed("tauri://localhost/", false));
    assert(OriginValidator::is_allowed("tauri://localhost/index.html", false));
    assert(OriginValidator::is_allowed("https://tauri.localhost", false));
    assert(OriginValidator::is_allowed("https://tauri.localhost/main.js", false));

    // Release mode must DENY localhost and remote
    assert(!OriginValidator::is_allowed("http://localhost:31337/evil.html", false));
    assert(!OriginValidator::is_allowed("http://127.0.0.1:8080/", false));
    assert(!OriginValidator::is_allowed("http://localhost:1420/", false));
    assert(!OriginValidator::is_allowed("https://example.com/", false));

    // Userinfo bypasses MUST BE DENIED
    assert(!OriginValidator::is_allowed("http://localhost:80@evil.example/pwn.html", false));
    assert(!OriginValidator::is_allowed("http://localhost:80@evil.example/pwn.html", true, "http://localhost:80"));
    assert(!OriginValidator::is_allowed("http://127.0.0.1:x@attacker.example/", true, "http://127.0.0.1:80"));
    assert(!OriginValidator::is_allowed("https://tauri.localhost:x@attacker.example/", false));
    assert(!OriginValidator::is_allowed("tauri://localhost:x@evil.example/", false));

    // Lookalikes
    assert(!OriginValidator::is_allowed("http://localhost.evil.example/", true, "http://localhost:1420"));
    assert(!OriginValidator::is_allowed("http://localhostevil.example/", true, "http://localhost:1420"));

    // Dev mode matching
    assert(OriginValidator::is_allowed("http://localhost:1420/", true, "http://localhost:1420"));
    assert(OriginValidator::is_allowed("http://localhost:1420/index.html", true, "http://localhost:1420"));
    assert(OriginValidator::is_allowed("http://127.0.0.1:1420/", true, "http://localhost:1420"));
    assert(!OriginValidator::is_allowed("http://localhost:3000/", true, "http://localhost:1420")); // wrong port

    // Empty / malformed
    assert(!OriginValidator::is_allowed("", false));
    assert(!OriginValidator::is_allowed("", true));

    std::cout << "All OriginValidator tests PASSED!\n";
    return 0;
}
