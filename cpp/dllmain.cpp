#include <Windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    constexpr uintptr_t RVA_GetAbramsAssetManager = 0x000FFDD20;
    constexpr uintptr_t RVA_RegisterSoftObjectPathList = 0x0014894E0;
    constexpr uintptr_t RVA_AddSoftObjectPath = 0x0014895D0;
    constexpr uintptr_t RVA_CommitPendingSoftPaths = 0x0014BF520;
    constexpr uintptr_t RVA_BuildSoftPathKey = 0x0018F8230;
    constexpr uintptr_t RVA_BuildSoftPathFromString = 0x0018F8530;

    constexpr uintptr_t RVA_DlcRegisterListCallerA = 0x001591B39;
    constexpr uintptr_t RVA_DlcRegisterListCallerB = 0x0013CDB83;

    constexpr uintptr_t kAbramsAssetManagerCommitFlagOffset = 0x450;
    constexpr std::size_t kRegisterListStolenBytes = 16;

    using GetAbramsAssetManagerFn = void* (*)();
    using RegisterSoftObjectPathListFn = void (*)(void* asset_manager, std::int64_t* path_array);
    using AddSoftObjectPathFn = void (*)(void* asset_manager, std::uint64_t* soft_path);
    using CommitPendingSoftPathsFn = void (*)(void* asset_manager);
    using BuildSoftPathKeyFn =
        std::uint64_t* (*)(std::uint64_t* out, std::uint32_t path_length, const wchar_t* path,
                           std::uint32_t subobject_index);
    using BuildSoftPathFromStringFn =
        std::uint64_t* (*)(std::uint64_t* out, const wchar_t* path, std::uint32_t subobject_index);

    std::atomic<bool> g_registered{false};
    std::atomic<bool> g_registering{false};
    std::atomic<bool> g_hooks_installed{false};
    RegisterSoftObjectPathListFn g_original_register_list = nullptr;
    std::vector<std::wstring> g_cached_paths;
    std::mutex g_log_mutex;

    auto game_base() -> uintptr_t
    {
        return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    }

    template <typename T>
    auto game_fn(uintptr_t rva) -> T
    {
        return reinterpret_cast<T>(game_base() + rva);
    }

    auto caller_rva() -> uintptr_t
    {
        return reinterpret_cast<uintptr_t>(_ReturnAddress()) - game_base();
    }

    auto is_abs_jmp12_to(void* at, void* detour) -> bool
    {
        const auto* bytes = reinterpret_cast<const uint8_t*>(at);
        if (bytes[0] != 0x48 || bytes[1] != 0xB8 || bytes[10] != 0xFF || bytes[11] != 0xE0)
        {
            return false;
        }

        const auto target = *reinterpret_cast<const uintptr_t*>(bytes + 2);
        return target == reinterpret_cast<uintptr_t>(detour);
    }

    auto get_mod_root() -> std::filesystem::path
    {
        wchar_t dll_path[MAX_PATH]{};
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&get_mod_root), &self);
        GetModuleFileNameW(self, dll_path, MAX_PATH);
        return std::filesystem::path(dll_path).parent_path().parent_path();
    }

    auto log_file_path() -> std::filesystem::path
    {
        return get_mod_root() / "register.log";
    }

    auto begin_log_session() -> void
    {
        std::lock_guard lock(g_log_mutex);
        std::wofstream out(log_file_path(), std::ios::app);
        out << L"\n=== AbramsAssetRegistrar session pid=" << GetCurrentProcessId() << L" ===\n";
    }

    auto log_line(const std::wstring& line) -> void
    {
        OutputDebugStringW((line + L"\n").c_str());

        std::lock_guard lock(g_log_mutex);
        std::wofstream out(log_file_path(), std::ios::app);
        out << line << L'\n';
        out.flush();
    }

    auto write_abs_jump12(void* at, void* to) -> void
    {
        uint8_t patch[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};
        *reinterpret_cast<std::uint64_t*>(&patch[2]) = reinterpret_cast<std::uint64_t>(to);

        DWORD old_protect = 0;
        VirtualProtect(at, sizeof(patch), PAGE_EXECUTE_READWRITE, &old_protect);
        memcpy(at, patch, sizeof(patch));
        VirtualProtect(at, sizeof(patch), old_protect, &old_protect);
        FlushInstructionCache(GetCurrentProcess(), at, sizeof(patch));
    }

    auto install_hook(void* target, void* detour, std::size_t stolen_bytes, void** trampoline_out) -> bool
    {
        if (stolen_bytes < 12)
        {
            return false;
        }

        if (is_abs_jmp12_to(target, detour))
        {
            log_line(L"[AbramsAssetRegistrar] hook already present, skipping re-patch");
            return false;
        }

        void* trampoline = VirtualAlloc(nullptr, stolen_bytes + 12, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!trampoline)
        {
            return false;
        }

        memcpy(trampoline, target, stolen_bytes);
        write_abs_jump12(reinterpret_cast<std::uint8_t*>(trampoline) + stolen_bytes,
                         reinterpret_cast<std::uint8_t*>(target) + stolen_bytes);
        write_abs_jump12(target, detour);
        *trampoline_out = trampoline;
        return true;
    }

    auto resolve_paths_file(const std::filesystem::path& mod_root) -> std::filesystem::path
    {
        const auto json_path = mod_root / "paths.json";
        if (std::filesystem::exists(json_path))
        {
            return json_path;
        }
        return mod_root / "paths.txt";
    }

    auto utf8_to_wide(const std::string& text) -> std::wstring
    {
        if (text.empty())
        {
            return {};
        }

        const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
        std::wstring wide(static_cast<size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), size);
        if (!wide.empty() && wide.back() == L'\0')
        {
            wide.pop_back();
        }
        return wide;
    }

    auto load_paths_txt(const std::filesystem::path& path_file) -> std::vector<std::wstring>
    {
        std::vector<std::wstring> paths;
        std::ifstream input(path_file);
        std::string line;

        while (std::getline(input, line))
        {
            if (line.empty() || line[0] == '#')
            {
                continue;
            }
            paths.push_back(utf8_to_wide(line));
        }

        return paths;
    }

    auto load_paths_json(const std::filesystem::path& path_file) -> std::vector<std::wstring>
    {
        std::vector<std::wstring> paths;
        std::ifstream input(path_file);
        const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());

        std::size_t pos = 0;
        while ((pos = content.find("/Game/", pos)) != std::string::npos)
        {
            const std::size_t end = content.find('"', pos);
            if (end == std::string::npos)
            {
                break;
            }
            paths.push_back(utf8_to_wide(content.substr(pos, end - pos)));
            pos = end + 1;
        }

        return paths;
    }

    auto cache_paths() -> void
    {
        if (!g_cached_paths.empty())
        {
            return;
        }

        const auto paths_file = resolve_paths_file(get_mod_root());
        if (!std::filesystem::exists(paths_file))
        {
            log_line(L"[AbramsAssetRegistrar] Missing paths file: " + paths_file.wstring());
            return;
        }

        g_cached_paths = paths_file.extension() == L".json" ? load_paths_json(paths_file) : load_paths_txt(paths_file);
        log_line(L"[AbramsAssetRegistrar] Cached " + std::to_wstring(g_cached_paths.size()) + L" paths");
    }

    auto get_asset_manager_safe(void* hinted_manager) -> void*
    {
        if (hinted_manager != nullptr)
        {
            return hinted_manager;
        }

        __try
        {
            return game_fn<GetAbramsAssetManagerFn>(RVA_GetAbramsAssetManager)();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    auto build_soft_path_safe(std::uint64_t* out, const wchar_t* object_path) -> bool
    {
        __try
        {
            const auto path_length = static_cast<std::uint32_t>(wcslen(object_path));
            if (path_length == 0)
            {
                return false;
            }

            auto build_key = game_fn<BuildSoftPathKeyFn>(RVA_BuildSoftPathKey);
            auto build_string = game_fn<BuildSoftPathFromStringFn>(RVA_BuildSoftPathFromString);

            for (const auto subobject_index : {1u, 0u})
            {
                memset(out, 0, sizeof(std::uint64_t) * 4);
                build_key(out, path_length, object_path, subobject_index);
                if (out[0] != 0)
                {
                    return true;
                }

                memset(out, 0, sizeof(std::uint64_t) * 4);
                build_string(out, object_path, subobject_index);
                if (out[0] != 0)
                {
                    return true;
                }
            }

            return false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    auto add_soft_path_safe(void* asset_manager, std::uint64_t* soft_path) -> bool
    {
        __try
        {
            game_fn<AddSoftObjectPathFn>(RVA_AddSoftObjectPath)(asset_manager, soft_path);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    auto commit_pending_paths_safe(void* asset_manager) -> bool
    {
        __try
        {
            auto* const commit_flag = reinterpret_cast<std::uint8_t*>(reinterpret_cast<std::uintptr_t>(asset_manager) +
                                                                    kAbramsAssetManagerCommitFlagOffset);
            if (*commit_flag != 1)
            {
                *commit_flag = 1;
            }

            game_fn<CommitPendingSoftPathsFn>(RVA_CommitPendingSoftPaths)(asset_manager);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    auto register_mod_paths(void* asset_manager) -> void
    {
        if (g_registered.load() || g_registering.load() || g_cached_paths.empty())
        {
            return;
        }

        asset_manager = get_asset_manager_safe(asset_manager);
        if (asset_manager == nullptr)
        {
            log_line(L"[AbramsAssetRegistrar] AbramsAssetManager not ready");
            return;
        }

        g_registering.store(true);

        wchar_t header[96]{};
        swprintf_s(header, L"[AbramsAssetRegistrar] registering on game thread (caller=0x%llX)",
                   static_cast<unsigned long long>(caller_rva()));
        log_line(header);

        std::size_t registered = 0;
        std::size_t build_failed = 0;
        std::size_t add_failed = 0;

        for (const auto& path : g_cached_paths)
        {
            alignas(16) std::uint64_t soft_path[4]{};
            if (!build_soft_path_safe(soft_path, path.c_str()))
            {
                ++build_failed;
                continue;
            }

            if (add_soft_path_safe(asset_manager, soft_path))
            {
                ++registered;
            }
            else
            {
                ++add_failed;
            }
        }

        g_registering.store(false);

        log_line(L"[AbramsAssetRegistrar] Registered " + std::to_wstring(registered) + L" / " +
                 std::to_wstring(g_cached_paths.size()) + L" soft paths (" + std::to_wstring(build_failed) +
                 L" key build failures, " + std::to_wstring(add_failed) + L" add failures)");

        if (registered > 0)
        {
            if (!commit_pending_paths_safe(asset_manager))
            {
                log_line(L"[AbramsAssetRegistrar] commit_pending_soft_paths failed");
            }
            g_registered.store(true);
        }
    }

    auto is_dlc_register_list_caller(uintptr_t rva) -> bool
    {
        return rva == RVA_DlcRegisterListCallerA || rva == RVA_DlcRegisterListCallerB;
    }

    void hooked_register_soft_object_path_list(void* asset_manager, std::int64_t* path_array)
    {
        const uintptr_t return_rva = caller_rva();

        g_original_register_list(asset_manager, path_array);

        if (g_registered.load() || g_registering.load())
        {
            return;
        }

        if (is_dlc_register_list_caller(return_rva))
        {
            log_line(L"[AbramsAssetRegistrar] Skipping registration after DLC register_list");
            return;
        }

        register_mod_paths(asset_manager);
    }

    auto install_hooks() -> void
    {
        if (g_hooks_installed.exchange(true))
        {
            log_line(L"[AbramsAssetRegistrar] hooks already installed, skipping");
            return;
        }

        const auto base = game_base();
        wchar_t base_text[32]{};
        swprintf_s(base_text, L"%llX", static_cast<unsigned long long>(base));
        log_line(std::wstring(L"[AbramsAssetRegistrar] game base=0x") + base_text);

        cache_paths();

        void* register_target = reinterpret_cast<void*>(base + RVA_RegisterSoftObjectPathList);
        void* register_trampoline = nullptr;
        if (install_hook(register_target, reinterpret_cast<void*>(&hooked_register_soft_object_path_list),
                         kRegisterListStolenBytes, &register_trampoline))
        {
            g_original_register_list = reinterpret_cast<RegisterSoftObjectPathListFn>(register_trampoline);
            log_line(L"[AbramsAssetRegistrar] Hook installed on register_soft_object_path_list");
        }
        else if (!g_original_register_list)
        {
            log_line(L"[AbramsAssetRegistrar] Failed to hook register_soft_object_path_list");
        }
    }
} // namespace

extern "C"
{
    __declspec(dllexport) void* start_mod()
    {
        begin_log_session();
        log_line(L"[AbramsAssetRegistrar] start_mod");
        install_hooks();
        return nullptr;
    }

    __declspec(dllexport) void uninstall_mod(void*)
    {
    }
}
