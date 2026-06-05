-- sh4xe.example.lua -- demonstrates the Lua <-> SDK bridge.
--
-- Copy to "<game>/scripts/sh4xe.lua" to have it auto-run at startup. The runtime
-- exposes a global `sh4` table plus `sh4.sym` (the validated address table from
-- tools/sdk/generate_lua_symbols.py, deployed as scripts/sh4_symbols.lua).
--
-- Memory primitives (addresses are plain integers; reads are page-validated and
-- return nil + message on a bad address, so a typo won't crash the game):
--     sh4.read_u8/u16/u32/i32/f32/f64(addr)   -> value [, err]
--     sh4.write_u8/u16/u32/i32/f32(addr, val) -> ok(bool)
--     sh4.read_bytes(addr, n)                 -> string|nil
--     sh4.call_cdecl(addr, ...intargs)        -> int   (EXPERIMENTAL, see runtime)

-- Read the engine's frame-rate the safe way: by symbol, not raw address.
local fps = sh4.read_i32(sh4.sym.kGameFrameRateValue)
sh4xe_log("sh4: g_frameRate = " .. tostring(fps))

-- Call a validated getter through the function table.
local rate = sh4.call_cdecl(sh4.sym.kGameGetFrameRate)
sh4xe_log("sh4: Game_GetFrameRate() returned " .. tostring(rate))

-- Inspect the render/animation per-frame seconds float.
local secs = sh4.read_f32(sh4.sym.kRenderFrameSecondsFloat)
sh4xe_log("sh4: render frame seconds = " .. tostring(secs))

-- List everything the SDK currently considers validated.
for name, addr in pairs(sh4.sym) do
    sh4xe_log(string.format("sh4.sym.%s = 0x%08X", name, addr))
end
