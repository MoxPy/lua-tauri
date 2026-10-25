local ffi = require("ffi")

local os_name = ffi.os
local app_name = "tauri-lua-app"
local version = "1.0.0"

local function shell_quote(str)
    local s = tostring(str)
    if ffi.os == "Windows" then
        s = s:gsub("%%", "^%%")
        s = s:gsub('(\\*)"', '%1%1\\"')
        local trailing_bs = s:match("(\\+)$")
        if trailing_bs then
            s = s .. trailing_bs
        end
        return '"' .. s .. '"'
    else
        return "'" .. s:gsub("'", "'\\''") .. "'"
    end
end

local function xml_escape(s)
    return tostring(s or "")
        :gsub("&", "&amp;")
        :gsub("<", "&lt;")
        :gsub(">", "&gt;")
        :gsub('"', "&quot;")
        :gsub("'", "&apos;")
end

local function exec(cmd)
    local ret = os.execute(cmd)
    if ret ~= 0 and ret ~= true then
        error("Failed command: " .. cmd)
    end
end

local function file_exists(path)
    local f = io.open(path, "r")
    if f then f:close(); return true end
    return false
end

local function detect_arch()
    local p = io.popen("uname -m 2>/dev/null")
    if p then
        local raw = p:read("*l")
        p:close()
        if raw and raw ~= "" then
            raw = raw:gsub("%s+", ""):lower()
            if raw == "x86_64" or raw == "amd64" then return "x86_64", "amd64" end
            if raw == "arm64" or raw == "aarch64" then return "arm64", "arm64" end
            if raw:match("^armv7") then return "armhf", "armhf" end
            return raw, raw
        end
    end
    return "x86_64", "amd64"
end

if os_name == "OSX" then
    local raw_arch = detect_arch()
    local bundle_dir = "build/" .. app_name .. ".app"
    local macos_dir = bundle_dir .. "/Contents/MacOS"
    local res_dir = bundle_dir .. "/Contents/Resources"
    local fw_dir = bundle_dir .. "/Contents/Frameworks"
    exec("mkdir -p " .. shell_quote(macos_dir))
    exec("mkdir -p " .. shell_quote(res_dir))
    exec("mkdir -p " .. shell_quote(fw_dir))
    exec("cp build/tauri_app " .. shell_quote(macos_dir .. "/" .. app_name))
    if file_exists("build/libtaurilua.dylib") then
        exec("cp build/libtaurilua.dylib " .. shell_quote(fw_dir .. "/"))
    end
    exec("cp -r dist " .. shell_quote(res_dir .. "/"))
    
    local plist = string.format([[<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>%s</string>
    <key>CFBundleIdentifier</key>
    <string>com.tauri.lua</string>
    <key>CFBundleName</key>
    <string>%s</string>
    <key>CFBundleDisplayName</key>
    <string>%s</string>
    <key>CFBundleVersion</key>
    <string>%s</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
</dict>
</plist>]], xml_escape(app_name), xml_escape(app_name), xml_escape(app_name), xml_escape(version))
    
    local f = io.open(bundle_dir .. "/Contents/Info.plist", "w")
    if f then
        f:write(plist)
        f:close()
    end
    local dmg_path = string.format("build/%s_%s_%s.dmg", app_name, version, raw_arch)
    local diskutil_cmd = "diskutil image create from --volumeName " .. shell_quote(app_name) .. " --format UDZO " .. shell_quote(bundle_dir) .. " " .. shell_quote(dmg_path) .. " > /dev/null 2>&1"
    if os.execute(diskutil_cmd) ~= 0 then
        exec("hdiutil create -volname " .. shell_quote(app_name) .. " -srcfolder " .. shell_quote(bundle_dir) .. " -ov -format UDZO " .. shell_quote(dmg_path) .. " > /dev/null 2>&1")
    end

elseif os_name == "Linux" then
    local _, deb_arch = detect_arch()
    local deb_dir = string.format("build/%s_%s_%s", app_name, version, deb_arch)
    exec("mkdir -p " .. shell_quote(deb_dir .. "/DEBIAN"))
    exec("mkdir -p " .. shell_quote(deb_dir .. "/usr/bin"))
    exec("mkdir -p " .. shell_quote(deb_dir .. "/usr/lib"))
    exec("mkdir -p " .. shell_quote(deb_dir .. "/usr/share/" .. app_name .. "/dist"))
    exec("cp build/tauri_app " .. shell_quote(deb_dir .. "/usr/bin/" .. app_name))
    if file_exists("build/libtaurilua.so") then
        exec("cp build/libtaurilua.so " .. shell_quote(deb_dir .. "/usr/lib/"))
    end
    exec("cp -r dist/* " .. shell_quote(deb_dir .. "/usr/share/" .. app_name .. "/dist/"))
    
    local control = string.format([[Package: %s
Version: %s
Section: utils
Priority: optional
Architecture: %s
Depends: libgtk-3-0, libwebkit2gtk-4.1-0 | libwebkit2gtk-4.0-37, luajit | libluajit-5.1-2, libcurl4 | libcurl3-gnutls, libsqlite3-0
Recommends: libnotify-bin
Maintainer: Tauri-Lua Contributors <noreply@github.com>
Description: Tauri-Lua - LuaJIT-powered desktop application runtime
]], app_name, version, deb_arch)
    
    local f = io.open(deb_dir .. "/DEBIAN/control", "w")
    if f then
        f:write(control)
        f:close()
    end
    exec("dpkg-deb --build " .. shell_quote(deb_dir))

elseif os_name == "Windows" then
    local out_dir = "build\\" .. app_name .. "-standalone"
    exec('mkdir "' .. out_dir .. '" 2>NUL || rem')
    exec('copy build\\Release\\tauri_app.exe "' .. out_dir .. '\\" 2>NUL || copy build\\tauri_app.exe "' .. out_dir .. '\\"')
    if file_exists("build\\Release\\taurilua.dll") then
        exec('copy build\\Release\\taurilua.dll "' .. out_dir .. '\\"')
    elseif file_exists("build\\taurilua.dll") then
        exec('copy build\\taurilua.dll "' .. out_dir .. '\\"')
    end
    exec('copy build\\Release\\lua51.dll "' .. out_dir .. '\\" 2>NUL || copy build\\lua51.dll "' .. out_dir .. '\\" 2>NUL || copy build\\Release\\luajit.dll "' .. out_dir .. '\\" 2>NUL || copy build\\luajit.dll "' .. out_dir .. '\\" 2>NUL || rem')
    local luajit_env = os.getenv("LUAJIT_ROOT")
    if luajit_env then
        exec('copy "' .. luajit_env .. '\\bin\\lua51.dll" "' .. out_dir .. '\\" 2>NUL || copy "' .. luajit_env .. '\\lua51.dll" "' .. out_dir .. '\\" 2>NUL || rem')
    end
    exec('xcopy /E /I dist "' .. out_dir .. '\\dist"')
    exec('tar -czf "build\\' .. app_name .. '-windows-x64.zip" -C build "' .. app_name .. '-standalone"')
end