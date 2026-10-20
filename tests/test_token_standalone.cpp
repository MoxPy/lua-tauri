#include "src/ipc_envelope.hpp"
#include <iostream>
#include <cassert>
#include <string>

int main() {
    std::string tok = "secret123";
    std::string valid = "[\"cmd\",{\"data\":\"ok\"},\"secret123\"]";
    std::string valid_spaces = "[\"cmd\", {\"data\":\"ok\"} , \"secret123\" ]";
    std::string fake_in_payload = "[\"cmd\",{\"data\":\"secret123\"},\"wrong\"]";
    std::string no_token = "[\"cmd\",{\"data\":\"secret123\"}]";
    std::string escaped_quote = "[\"cmd\",{\"msg\":\"hello \\\"world\\\"\"},\"secret123\"]";

    assert(verify_envelope_token(valid, tok));
    assert(verify_envelope_token(valid_spaces, tok));
    assert(!verify_envelope_token(fake_in_payload, tok));
    assert(!verify_envelope_token(no_token, tok));
    assert(verify_envelope_token(escaped_quote, tok));

    // constant_time_eq checks
    assert(constant_time_eq("secret123", "secret123"));
    assert(!constant_time_eq("secret123", "secret124"));
    assert(!constant_time_eq("secret123", "secret12"));
    assert(!constant_time_eq("", "secret123"));

    // extract_json_string checks
    std::string json = "{\"name\":\"tauri:echo\",\"seq\":\"42\",\"req\":\"{\\\"hello\\\":\\\"world\\\"}\"}";
    assert(extract_json_string(json, "name") == "tauri:echo");
    assert(extract_json_string(json, "seq") == "42");
    assert(extract_json_string(json, "req") == "{\"hello\":\"world\"}");
    assert(extract_json_string(json, "unknown") == "");

    std::cout << "All standalone token & envelope tests PASSED!\n";
    return 0;
}
