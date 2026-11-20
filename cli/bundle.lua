local script_dir = arg and arg[0] and arg[0]:match("^(.*)[/\\][^/\\]+$") or "cli"
arg[1] = "build"
dofile(script_dir .. "/tauri.lua")
