-- Probe for MPC's hidden Lua scripting: proves that MPC ran this file, under which name, and lists
-- what the scripting environment exposes. Writes /tmp/mpc-script-probe-<name>.txt on the MPC.
-- It calls nothing of MPC's: it only reads the global table and logs the device callbacks.
-- The deploy step replaces PROBE_NAME with the file's own name.

local NAME = "PROBE_NAME"
local out = io and io.open("/tmp/mpc-script-probe-" .. NAME .. ".txt", "a")

local function log(s)
    if out then out:write(s, "\n"); out:flush() end
    print("[midimix probe " .. NAME .. "] " .. s)
end

log("loaded")

local STD = {string = true, table = true, math = true, io = true, coroutine = true, package = true,
             utf8 = true, os = true, debug = true, _G = true}
local seen = {}

local function keys_of(t)
    local ks = {}
    for k in pairs(t) do ks[#ks + 1] = k end
    table.sort(ks, function(a, b) return tostring(a) < tostring(b) end)
    return ks
end

local function dump(v, path, depth)
    local kind = type(v)
    log(path .. " : " .. kind)
    if depth >= 4 or seen[v] then return end
    if kind == "table" then
        seen[v] = true
        for _, k in ipairs(keys_of(v)) do
            if not (depth == 0 and STD[k]) then dump(v[k], path .. "." .. tostring(k), depth + 1) end
        end
    elseif kind == "userdata" then
        seen[v] = true
        local ok, mt = pcall(getmetatable, v)
        if ok and type(mt) == "table" then
            for _, k in ipairs(keys_of(mt)) do log(path .. " [meta] " .. tostring(k)) end
            if type(mt.__index) == "table" then dump(mt.__index, path .. "[index]", depth + 1) end
        end
    end
end

dump(_G, "_G", 0)
log("dump done")

function OnLoad(...) log("OnLoad called, " .. select("#", ...) .. " args") end
function OnUnload(...) log("OnUnload called") end
function HandleMIDINoteOn(...) log("HandleMIDINoteOn " .. table.concat({...}, " ")) end
function HandleMIDINoteOff(...) log("HandleMIDINoteOff " .. table.concat({...}, " ")) end
function HandleMIDICC(...) log("HandleMIDICC " .. table.concat({...}, " ")) end
