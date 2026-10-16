local ffi = require("ffi")

local os_name = ffi.os
local app_name = "tauri-lua-app"
local version = "1.0.0"

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
    exec("mkdir -p " .. macos_dir)
    exec("mkdir -p " .. res_dir)
    exec("mkdir -p " .. fw_dir)
    exec("cp build/tauri_app " .. macos_dir .. "/" .. app_name)
    if file_exists("build/libtaurilua.dylib") then
        exec("cp build/libtaurilua.dylib " .. fw_dir .. "/")
    end
    exec("cp -r dist " .. res_dir .. "/")
    
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
    <key>CFBundleVersion</key>
    <string>%s</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
</dict>
</plist>]], app_name, app_name, version)
    
    local f = io.open(bundle_dir .. "/Contents/Info.plist", "w")
    f:write(plist)
    f:close()
    local dmg_path = string.format("build/%s_%s_%s.dmg", app_name, version, raw_arch)
    exec("hdiutil create -volname " .. app_name .. " -srcfolder " .. bundle_dir .. " -ov -format UDZO " .. dmg_path)

elseif os_name == "Linux" then
    local _, deb_arch = detect_arch()
    local deb_dir = string.format("build/%s_%s_%s", app_name, version, deb_arch)
    exec("mkdir -p " .. deb_dir .. "/DEBIAN")
    exec("mkdir -p " .. deb_dir .. "/usr/bin")
    exec("mkdir -p " .. deb_dir .. "/usr/lib")
    exec("mkdir -p " .. deb_dir .. "/usr/share/" .. app_name .. "/dist")
    exec("cp build/tauri_app " .. deb_dir .. "/usr/bin/" .. app_name)
    if file_exists("build/libtaurilua.so") then
        exec("cp build/libtaurilua.so " .. deb_dir .. "/usr/lib/")
    end
    exec("cp -r dist/* " .. deb_dir .. "/usr/share/" .. app_name .. "/dist/")
    
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
    f:write(control)
    f:close()
    exec("dpkg-deb --build " .. deb_dir)

elseif os_name == "Windows" then
    local out_dir = "build\\" .. app_name .. "-standalone"
    exec("mkdir " .. out_dir)
    exec("copy build\\Release\\tauri_app.exe " .. out_dir .. "\\")
    if file_exists("build\\Release\\taurilua.dll") then
        exec("copy build\\Release\\taurilua.dll " .. out_dir .. "\\")
    elseif file_exists("build\\taurilua.dll") then
        exec("copy build\\taurilua.dll " .. out_dir .. "\\")
    end
    exec("xcopy /E /I dist " .. out_dir .. "\\dist")
    exec("tar -czf build\\" .. app_name .. "-windows-x64.zip -C build " .. app_name .. "-standalone")
end