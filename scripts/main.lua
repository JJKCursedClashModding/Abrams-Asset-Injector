local function mod_root_from_script()
    local src = debug.getinfo(1, "S").source
    if src:sub(1, 1) == "@" then
        src = src:sub(2)
    end
    local scripts_dir = src:match("^(.*[\\/])") or ""
    return scripts_dir:match("^(.*)[\\/][^\\/]+[\\/]?$") or scripts_dir
end

local MOD_ROOT = mod_root_from_script()
local register_paths = package.loadlib(MOD_ROOT .. "\\dlls\\main.dll", "abrams_register_paths")

if not register_paths then
    print("[AbramsAssetRegistrar] package.loadlib failed\n")
    return
end

-- C++ return codes from abrams_register_paths
local RESULT_FAILED = 0
local RESULT_SUCCESS = 1
local RESULT_NOT_READY = 2

local registration_done = false
local registration_pending = false
local registration_gave_up = false

local function find_asset_manager()
    local mgr = FindFirstOf("AbramsAssetManager")
    if mgr and mgr:IsValid() then
        return mgr
    end
    return nil
end

LoopAsync(500, function()
    if registration_done or registration_gave_up then
        return true
    end

    if registration_pending then
        return false
    end

    local mgr = find_asset_manager()
    if not mgr then
        return false
    end

    local addr = mgr:GetAddress()
    registration_pending = true
    ExecuteInGameThread(function()
        local ok = register_paths(addr)
        registration_pending = false

        if ok == RESULT_SUCCESS then
            registration_done = true
            print("[AbramsAssetRegistrar] registration complete\n")
        elseif ok == RESULT_NOT_READY then
            -- startup register_soft_object_path_list not done yet; keep polling
        else
            registration_gave_up = true
            print("[AbramsAssetRegistrar] registration failed (see register.log)\n")
        end
    end)

    return registration_done
end)

print("[AbramsAssetRegistrar] Lua dispatcher loaded (LoopAsync poll)\n")
