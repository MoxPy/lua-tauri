-- RFC 8259 JSON parser and encoder verification test

local function create_json()
    local json = {}

    local function utf8_encode(cp)
        if cp < 0x80 then
            return string.char(cp)
        elseif cp < 0x800 then
            return string.char(0xC0 + math.floor(cp / 0x40), 0x80 + (cp % 0x40))
        elseif cp < 0x10000 then
            return string.char(0xE0 + math.floor(cp / 0x1000), 0x80 + (math.floor(cp / 0x40) % 0x40), 0x80 + (cp % 0x40))
        else
            return string.char(
                0xF0 + math.floor(cp / 0x40000),
                0x80 + (math.floor(cp / 0x1000) % 0x40),
                0x80 + (math.floor(cp / 0x40) % 0x40),
                0x80 + (cp % 0x40)
            )
        end
    end

    local escape_map = {
        ['"'] = '\\"',
        ['\\'] = '\\\\',
        ['\b'] = '\\b',
        ['\f'] = '\\f',
        ['\n'] = '\\n',
        ['\r'] = '\\r',
        ['\t'] = '\\t'
    }

    local function encode_string(s)
        return '"' .. s:gsub('[%z\1-\31\\"]', function(c)
            return escape_map[c] or string.format('\\u%04x', string.byte(c))
        end) .. '"'
    end

    function json.encode(val, visited)
        visited = visited or {}
        local t = type(val)
        if t == "nil" then
            return "null"
        elseif t == "boolean" then
            return val and "true" or "false"
        elseif t == "number" then
            if val ~= val then
                error("JSON encode error: NaN is not allowed in RFC 8259")
            elseif val == math.huge or val == -math.huge then
                error("JSON encode error: Infinity is not allowed in RFC 8259")
            end
            return tostring(val)
        elseif t == "string" then
            return encode_string(val)
        elseif t == "table" then
            if visited[val] then
                error("JSON encode error: circular reference detected")
            end
            visited[val] = true

            local is_array = true
            local max_idx = 0
            local count = 0
            for k, _ in pairs(val) do
                count = count + 1
                if type(k) == "number" and k > 0 and math.floor(k) == k then
                    if k > max_idx then max_idx = k end
                else
                    is_array = false
                end
            end

            -- Empty table: treat as {} unless tagged as array
            if count == 0 then
                visited[val] = nil
                return "{}"
            end

            if is_array and max_idx == count then
                local parts = {}
                for i = 1, max_idx do
                    parts[i] = json.encode(val[i], visited)
                end
                visited[val] = nil
                return "[" .. table.concat(parts, ",") .. "]"
            else
                local parts = {}
                for k, v in pairs(val) do
                    table.insert(parts, encode_string(tostring(k)) .. ":" .. json.encode(v, visited))
                end
                visited[val] = nil
                return "{" .. table.concat(parts, ",") .. "}"
            end
        else
            error("JSON encode error: unsupported type " .. t)
        end
    end

    function json.decode(str)
        if type(str) ~= "string" or str == "" then
            return nil, "empty input"
        end

        local len = #str
        local pos = 1
        local depth = 0
        local max_depth = 128

        local function skip_ws()
            while pos <= len do
                local c = str:sub(pos, pos)
                if c == " " or c == "\t" or c == "\r" or c == "\n" then
                    pos = pos + 1
                else
                    break
                end
            end
        end

        local parse_value

        local function parse_string()
            pos = pos + 1 -- skip opening "
            local chunks = {}
            local start = pos
            while pos <= len do
                local c = str:sub(pos, pos)
                if c == '"' then
                    if pos > start then
                        table.insert(chunks, str:sub(start, pos - 1))
                    end
                    pos = pos + 1
                    return table.concat(chunks)
                elseif c == '\\' then
                    if pos > start then
                        table.insert(chunks, str:sub(start, pos - 1))
                    end
                    pos = pos + 1
                    if pos > len then return nil, "unterminated escape sequence" end
                    local esc = str:sub(pos, pos)
                    if esc == '"' or esc == '\\' or esc == '/' then
                        table.insert(chunks, esc)
                        pos = pos + 1
                    elseif esc == 'b' then
                        table.insert(chunks, "\b")
                        pos = pos + 1
                    elseif esc == 'f' then
                        table.insert(chunks, "\f")
                        pos = pos + 1
                    elseif esc == 'n' then
                        table.insert(chunks, "\n")
                        pos = pos + 1
                    elseif esc == 'r' then
                        table.insert(chunks, "\r")
                        pos = pos + 1
                    elseif esc == 't' then
                        table.insert(chunks, "\t")
                        pos = pos + 1
                    elseif esc == 'u' then
                        pos = pos + 1
                        if pos + 3 > len then return nil, "incomplete \\uXXXX escape" end
                        local hex = str:sub(pos, pos + 3)
                        if not hex:match("^[0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F]$") then
                            return nil, "invalid \\uXXXX escape: " .. hex
                        end
                        pos = pos + 4
                        local cp = tonumber(hex, 16)
                        if cp >= 0xD800 and cp <= 0xDBFF then
                            -- High surrogate, check for low surrogate
                            if str:sub(pos, pos + 1) == "\\u" then
                                local low_hex = str:sub(pos + 2, pos + 5)
                                if low_hex:match("^[0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F]$") then
                                    local low = tonumber(low_hex, 16)
                                    if low >= 0xDC00 and low <= 0xDFFF then
                                        pos = pos + 6
                                        cp = 0x10000 + ((cp - 0xD800) * 0x400) + (low - 0xDC00)
                                    end
                                end
                            end
                        end
                        table.insert(chunks, utf8_encode(cp))
                    else
                        return nil, "invalid escape char \\" .. esc
                    end
                    start = pos
                else
                    local b = string.byte(c)
                    if b < 32 then
                        return nil, "unescaped control character in string"
                    end
                    pos = pos + 1
                end
            end
            return nil, "unterminated string"
        end

        local function parse_object()
            pos = pos + 1 -- skip {
            depth = depth + 1
            if depth > max_depth then return nil, "maximum depth exceeded" end
            local obj = {}
            skip_ws()
            if pos <= len and str:sub(pos, pos) == '}' then
                pos = pos + 1
                depth = depth - 1
                return obj
            end

            while pos <= len do
                skip_ws()
                if pos > len then return nil, "unterminated object" end
                if str:sub(pos, pos) ~= '"' then
                    return nil, "expected string key in object at position " .. pos
                end
                local key, err = parse_string()
                if not key then return nil, err end

                skip_ws()
                if pos > len or str:sub(pos, pos) ~= ':' then
                    return nil, "expected ':' after key in object at position " .. pos
                end
                pos = pos + 1 -- skip :

                skip_ws()
                local val, val_err = parse_value()
                if val_err then return nil, val_err end
                obj[key] = val

                skip_ws()
                if pos <= len and str:sub(pos, pos) == '}' then
                    pos = pos + 1
                    depth = depth - 1
                    return obj
                elseif pos <= len and str:sub(pos, pos) == ',' then
                    pos = pos + 1
                    skip_ws()
                    if pos <= len and str:sub(pos, pos) == '}' then
                        return nil, "trailing comma in object"
                    end
                else
                    return nil, "expected ',' or '}' in object at position " .. pos
                end
            end
            return nil, "unterminated object"
        end

        local function parse_array()
            pos = pos + 1 -- skip [
            depth = depth + 1
            if depth > max_depth then return nil, "maximum depth exceeded" end
            local arr = {}
            skip_ws()
            if pos <= len and str:sub(pos, pos) == ']' then
                pos = pos + 1
                depth = depth - 1
                return arr
            end

            while pos <= len do
                skip_ws()
                local val, err = parse_value()
                if err then return nil, err end
                table.insert(arr, val)

                skip_ws()
                if pos <= len and str:sub(pos, pos) == ']' then
                    pos = pos + 1
                    depth = depth - 1
                    return arr
                elseif pos <= len and str:sub(pos, pos) == ',' then
                    pos = pos + 1
                    skip_ws()
                    if pos <= len and str:sub(pos, pos) == ']' then
                        return nil, "trailing comma in array"
                    end
                else
                    return nil, "expected ',' or ']' in array at position " .. pos
                end
            end
            return nil, "unterminated array"
        end

        local function parse_number()
            local start = pos
            if str:sub(pos, pos) == '-' then pos = pos + 1 end
            if pos > len then return nil, "invalid number" end
            if str:sub(pos, pos) == '0' then
                pos = pos + 1
            elseif str:sub(pos, pos):match("^[1-9]$") then
                while pos <= len and str:sub(pos, pos):match("^[0-9]$") do
                    pos = pos + 1
                end
            else
                return nil, "invalid number format at position " .. pos
            end

            if pos <= len and str:sub(pos, pos) == '.' then
                pos = pos + 1
                if pos > len or not str:sub(pos, pos):match("^[0-9]$") then
                    return nil, "invalid fractional number"
                end
                while pos <= len and str:sub(pos, pos):match("^[0-9]$") do
                    pos = pos + 1
                end
            end

            if pos <= len and (str:sub(pos, pos) == 'e' or str:sub(pos, pos) == 'E') then
                pos = pos + 1
                if pos <= len and (str:sub(pos, pos) == '+' or str:sub(pos, pos) == '-') then
                    pos = pos + 1
                end
                if pos > len or not str:sub(pos, pos):match("^[0-9]$") then
                    return nil, "invalid exponent in number"
                end
                while pos <= len and str:sub(pos, pos):match("^[0-9]$") do
                    pos = pos + 1
                end
            end

            local num_str = str:sub(start, pos - 1)
            local num = tonumber(num_str)
            if not num then return nil, "cannot convert number " .. num_str end
            return num
        end

        function parse_value()
            skip_ws()
            if pos > len then return nil, "unexpected end of input" end
            local c = str:sub(pos, pos)
            if c == '{' then
                return parse_object()
            elseif c == '[' then
                return parse_array()
            elseif c == '"' then
                return parse_string()
            elseif c == 't' then
                if str:sub(pos, pos + 3) == "true" then
                    pos = pos + 4
                    return true
                else
                    return nil, "unexpected token at position " .. pos
                end
            elseif c == 'f' then
                if str:sub(pos, pos + 4) == "false" then
                    pos = pos + 5
                    return false
                else
                    return nil, "unexpected token at position " .. pos
                end
            elseif c == 'n' then
                if str:sub(pos, pos + 3) == "null" then
                    pos = pos + 4
                    return nil
                else
                    return nil, "unexpected token at position " .. pos
                end
            elseif c == '-' or c:match("^[0-9]$") then
                return parse_number()
            else
                return nil, "unexpected character '" .. c .. "' at position " .. pos
            end
        end

        local res, err = parse_value()
        if err then return nil, err end
        skip_ws()
        if pos <= len then
            return nil, "trailing characters after JSON at position " .. pos
        end
        return res
    end

    return json
end

-- Test suite
local json = create_json()

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
