-- RFC 8259 JSON parser and encoder verification test
-- Tests real shipped JSON implementation from src/tauri/init.lua

package.path = "src/?.lua;src/?/init.lua;" .. package.path
local tauri = require("tauri")
local json = tauri.json or (tauri.new and tauri.new().json)

local function assert_eq(a, b, msg)
    if a ~= b then
        error(string.format("Assertion failed: '%s' ~= '%s' (%s)", tostring(a), tostring(b), msg or ""))
    end
end

-- 1. Malformed inputs that caused infinite loops in audited code
local t0 = os.clock()
local res, err = json.decode('[1,2')
assert(res == nil and err ~= nil, "Malformed array must fail quickly")
local res2, err2 = json.decode('{"a":1')
assert(res2 == nil and err2 ~= nil, "Malformed object must fail quickly")
local elapsed = os.clock() - t0
assert(elapsed < 0.1, "Malformed inputs must return in under 0.1s")

-- 2. Escape fidelity
local d1, _ = json.decode('"a\\\\nb"')
assert_eq(d1, "a\\nb", "Escape \\\\n must decode to a\\nb, not a newline")

local d2, _ = json.decode('"\\u0041"')
assert_eq(d2, "A", "\\u0041 must decode to A")

local d3, _ = json.decode('"x\\/y"')
assert_eq(d3, "x/y", "x\\/y must decode to x/y")

local d4, _ = json.decode('"\\uD83D\\uDE00"') -- Grinning face emoji 😀
assert_eq(d4, "😀", "Surrogate pair must decode to 😀")

-- 3. Reject unescaped control chars
local d5, err5 = json.decode('"\n"')
assert(d5 == nil and err5 ~= nil, "Unescaped control chars must be rejected")

-- 4. Encoder tests
assert_eq(json.encode({}), "{}", "Empty table must encode to {}")
assert_eq(json.encode({1, 2, 3}), "[1,2,3]", "Array must encode to [1,2,3]")

-- R2: Empty array preservation
local empty_arr = json.decode("[]")
assert_eq(json.encode(empty_arr), "[]", "Decoded empty array must re-encode to []")
assert_eq(json.encode(json.empty_array), "[]", "json.empty_array encodes to []")

-- R2: Null preservation and index integrity in arrays
local arr_with_null = json.decode("[1,null,3]")
assert(arr_with_null[1] == 1, "arr[1] is 1")
assert(arr_with_null[2] == json.null, "arr[2] is json.null")
assert(arr_with_null[3] == 3, "arr[3] is 3")
assert_eq(json.encode(arr_with_null), "[1,null,3]", "[1,null,3] round-trips without shifting indices")

-- R2: High-precision / large integer encoding without exponent
local big_int = 8944394323791464
assert_eq(json.encode(big_int), "8944394323791464", "Large integer encodes without scientific exponent")

local enc_str = json.encode("line1\nline2\t\"quoted\"")
assert_eq(enc_str, '"line1\\nline2\\t\\"quoted\\""', "String encoding with escapes")

-- 5. Cyclic table detection
local cycle = {}
cycle.self = cycle
local ok_cycle, _ = pcall(function() json.encode(cycle) end)
assert(not ok_cycle, "Circular reference must be detected and rejected")

-- 6. Reject NaN/Infinity
local ok_nan, _ = pcall(function() json.encode(0/0) end)
assert(not ok_nan, "NaN must be rejected")

print("All RFC 8259 JSON tests PASSED successfully!")
