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

if os_name == "OSX" then
    local bundle_dir = "build/" .. app_name .. ".app"
    local macos_dir = bundle_dir .. "/Contents/MacOS"
    local res_dir = bundle_dir .. "/Contents/Resources"
    exec("mkdir -p " .. macos_dir)
    exec("mkdir -p " .. res_dir)
    exec("cp build/tauri_app " .. macos_dir .. "/" .. app_name)
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
    exec("hdiutil create -volname " .. app_name .. " -srcfolder " .. bundle_dir .. " -ov -format UDZO build/" .. app_name .. ".dmg")

elseif os_name == "Linux" then
    local deb_dir = "build/" .. app_name .. "_" .. version .. "_amd64"
    exec("mkdir -p " .. deb_dir .. "/DEBIAN")
    exec("mkdir -p " .. deb_dir .. "/usr/bin")
    exec("mkdir -p " .. deb_dir .. "/usr/share/" .. app_name .. "/dist")
    exec("cp build/tauri_app " .. deb_dir .. "/usr/bin/" .. app_name)
    exec("cp -r dist/* " .. deb_dir .. "/usr/share/" .. app_name .. "/dist/")
    
    local control = string.format([[Package: %s
Version: %s
Section: utils
Priority: optional
Architecture: amd64
Maintainer: Mox
Description: Tauri runtime ported on LuaJIT
]], app_name, version)
    
    local f = io.open(deb_dir .. "/DEBIAN/control", "w")
    f:write(control)
    f:close()
    exec("dpkg-deb --build " .. deb_dir)

elseif os_name == "Windows" then
    local out_dir = "build\\" .. app_name .. "-standalone"
    exec("mkdir " .. out_dir)
    exec("copy build\\Release\\tauri_app.exe " .. out_dir .. "\\")
    exec("xcopy /E /I dist " .. out_dir .. "\\dist")
    exec("tar -czf build\\" .. app_name .. "-windows.tar.gz -C build " .. app_name .. "-standalone")
end