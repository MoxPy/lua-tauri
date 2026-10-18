#include <iostream>
#include <string>
#include <cctype>

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

int main() {
    std::string tok = "secret123";
    std::string valid = "[\"cmd\",{\"data\":\"ok\"},\"secret123\"]";
    std::string valid_spaces = "[\"cmd\", {\"data\":\"ok\"} , \"secret123\" ]";
    std::string fake_in_payload = "[\"cmd\",{\"data\":\"secret123\"},\"wrong\"]";
    std::string no_token = "[\"cmd\",{\"data\":\"secret123\"}]";

    std::cout << "valid: " << verify_envelope_token(valid, tok) << "\n";
    std::cout << "valid_spaces: " << verify_envelope_token(valid_spaces, tok) << "\n";
    std::cout << "fake_in_payload: " << verify_envelope_token(fake_in_payload, tok) << "\n";
    std::cout << "no_token: " << verify_envelope_token(no_token, tok) << "\n";
    return 0;
}
