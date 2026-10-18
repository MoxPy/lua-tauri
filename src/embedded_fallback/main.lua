-- Minimal fallback main.lua (deny-all default embedded in libtaurilua)
local tauri = require("tauri")
local app = tauri.new()
app:run()
