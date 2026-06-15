#include <Windows.h>
#include <intrin.h>

extern "C" {
#include "lua.h"
}

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
    constexpr uintptr_t RVA_GameInstanceAccessor = 0x001017BD0;

    constexpr uintptr_t kWrapperInnerOffset = 0x10;
    constexpr uintptr_t kGameInstanceManagerOffset = 0x220;
    constexpr uintptr_t kPendingSetCountOffset = 0x3B8;
    constexpr uintptr_t kPendingHashCapacityOffset = 0x3E4;
    constexpr uintptr_t kCommittedMapDataOffset = 0x400;
    constexpr uintptr_t kCommittedCountOffset = 0x408;
    constexpr uintptr_t kAbramsAssetManagerCommitFlagOffset = 0x450;
    constexpr std::uint32_t kExpectedPendingHashCapacity = 0x80;
    constexpr std::size_t kRegisterListStolenBytes = 16;

    constexpr std::size_t kDumpHeaderBytes = 0x200;
    constexpr std::size_t kDumpRegistryOffset = 0x380;
    constexpr std::size_t kDumpRegistryBytes = 0x120;

    constexpr int kLuaResultFailed = 0;
    constexpr int kLuaResultSuccess = 1;
    constexpr int kLuaResultNotReady = 2;

    using GetAbramsAssetManagerFn = void* (*)();
    using RegisterSoftObjectPathListFn = void (*)(void* asset_manager, void* path_array);
    using AddSoftObjectPathFn = void (*)(void* asset_manager, std::uint64_t* soft_path);
    using CommitPendingSoftPathsFn = void (*)(void* asset_manager);
    using BuildSoftPathKeyFn =
        std::uint64_t* (*)(std::uint64_t* out, std::uint32_t path_length, const wchar_t* path,
                           std::uint32_t subobject_index);
    using BuildSoftPathFromStringFn =
        std::uint64_t* (*)(std::uint64_t* out, const wchar_t* path, std::uint32_t subobject_index);
    using GameInstanceAccessorFn = void* (*)();

    struct SoftPathKeyArray
    {
        std::int64_t* data;
        std::int32_t count;
        std::int32_t capacity;
    };

    std::atomic<bool> g_registered{false};
    std::atomic<bool> g_registering{false};
    std::atomic<bool> g_hooks_installed{false};
    std::atomic<bool> g_game_bulk_register_complete{false};
    std::atomic<void*> g_hook_asset_manager{nullptr};
    RegisterSoftObjectPathListFn g_original_register_list = nullptr;
    std::vector<std::wstring> g_cached_paths;
    std::mutex g_log_mutex;
    void* g_dump_wrapper = nullptr;
    void* g_dump_inner = nullptr;
    std::atomic<int> g_defer_log_counter{0};

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

    auto manager_dump_file_path() -> std::filesystem::path
    {
        return get_mod_root() / "manager_dump.txt";
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

    auto is_abs_jmp12_to(void* at, void* detour) -> bool
    {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(at);
        if (bytes[0] != 0x48 || bytes[1] != 0xB8 || bytes[10] != 0xFF || bytes[11] != 0xE0)
        {
            return false;
        }

        const auto target = *reinterpret_cast<const uintptr_t*>(bytes + 2);
        return target == reinterpret_cast<uintptr_t>(detour);
    }

    auto write_abs_jump12(void* at, void* to) -> void
    {
        std::uint8_t patch[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};
        *reinterpret_cast<std::uint64_t*>(&patch[2]) = reinterpret_cast<std::uint64_t>(to);

        DWORD old_protect = 0;
        VirtualProtect(at, sizeof(patch), PAGE_EXECUTE_READWRITE, &old_protect);
        std::memcpy(at, patch, sizeof(patch));
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

        std::memcpy(trampoline, target, stolen_bytes);
        write_abs_jump12(reinterpret_cast<std::uint8_t*>(trampoline) + stolen_bytes,
                         reinterpret_cast<std::uint8_t*>(target) + stolen_bytes);
        write_abs_jump12(target, detour);
        *trampoline_out = trampoline;
        return true;
    }

    auto is_dlc_register_list_caller(uintptr_t rva) -> bool
    {
        return rva == RVA_DlcRegisterListCallerA || rva == RVA_DlcRegisterListCallerB;
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
        log_line(L"[AbramsAssetRegistrar] Cached " + std::to_wstring(g_cached_paths.size()) + L" paths from " +
                 paths_file.filename().wstring());
    }

    auto log_ptr(const wchar_t* label, const void* ptr) -> void
    {
        wchar_t buf[128]{};
        swprintf_s(buf, L"[AbramsAssetRegistrar] %s=0x%llX", label,
                   static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(ptr)));
        log_line(buf);
    }

    static auto seh_read_memory_block(const void* source, void* destination, std::size_t size) -> int
    {
        __try
        {
            std::memcpy(destination, source, size);
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    auto append_hex_dump(std::wofstream& out, const wchar_t* title, void* base, std::size_t offset,
                         std::size_t length) -> void
    {
        if (base == nullptr)
        {
            out << L"\n--- " << title << L" (null) ---\n";
            return;
        }

        wchar_t header[256]{};
        swprintf_s(header, L"\n--- %s @ 0x%llX (+0x%llX, %llu bytes) ---\n", title,
                   static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(base)),
                   static_cast<unsigned long long>(offset), static_cast<unsigned long long>(length));
        out << header;

        std::vector<std::uint8_t> bytes(length);
        const auto* const source = reinterpret_cast<const std::uint8_t*>(base) + offset;
        if (!seh_read_memory_block(source, bytes.data(), length))
        {
            out << L"READ FAILED (access violation)\n";
            return;
        }

        for (std::size_t row = 0; row < length; row += 16)
        {
            wchar_t line[320]{};
            const int prefix = swprintf_s(line, L"+0x%04llX: ",
                                          static_cast<unsigned long long>(offset + row));
            if (prefix <= 0)
            {
                continue;
            }

            std::wstring row_text(line);
            for (std::size_t column = 0; column < 16 && row + column < length; ++column)
            {
                wchar_t byte_text[8]{};
                swprintf_s(byte_text, L"%02X ", bytes[row + column]);
                row_text += byte_text;
            }

            if (row + 16 <= length)
            {
                wchar_t qwords[192]{};
                swprintf_s(qwords, L" | %016llX %016llX",
                           static_cast<unsigned long long>(
                               *reinterpret_cast<const std::uint64_t*>(bytes.data() + row)),
                           static_cast<unsigned long long>(
                               *reinterpret_cast<const std::uint64_t*>(bytes.data() + row + 8)));
                row_text += qwords;
            }

            out << row_text << L'\n';
        }
    }

    auto write_manager_memory_dump(void* selected_manager) -> void
    {
        const auto dump_path = manager_dump_file_path();
        std::wofstream out(dump_path, std::ios::trunc);
        if (!out)
        {
            log_line(L"[AbramsAssetRegistrar] failed to open manager_dump.txt for write");
            return;
        }

        out << L"=== AbramsAssetRegistrar manager memory dump pid=" << GetCurrentProcessId() << L" ===\n";
        out << L"add_soft_object_path uses manager+0x400 (FUN_1412a9b40) after startup register_list\n";
        out << L"probed fields: pending@+0x3B8 capacity@+0x3E4 committed@+0x408 table@+0x400 commit@+0x450\n";

        struct DumpTarget
        {
            const wchar_t* label;
            void* base;
        };

        const DumpTarget targets[] = {
            {L"GetAbrams wrapper (UObject)", g_dump_wrapper},
            {L"wrapper+0x10 inner", g_dump_inner},
            {L"selected native manager", selected_manager},
        };

        for (const auto& target : targets)
        {
            append_hex_dump(out, target.label, target.base, 0, kDumpHeaderBytes);
            append_hex_dump(out, target.label, target.base, kDumpRegistryOffset, kDumpRegistryBytes);
        }

        out.flush();
        log_line(L"[AbramsAssetRegistrar] wrote " + dump_path.wstring());
    }

    static auto seh_call_get_abrams_asset_manager(void** wrapper_out, DWORD* exception_code_out) -> int
    {
        __try
        {
            *wrapper_out = game_fn<GetAbramsAssetManagerFn>(RVA_GetAbramsAssetManager)();
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (exception_code_out != nullptr)
            {
                *exception_code_out = GetExceptionCode();
            }
            return 0;
        }
    }

    static auto seh_read_pointer_at(void* base, std::uintptr_t offset, void** value_out, DWORD* exception_code_out)
        -> int
    {
        __try
        {
            *value_out = *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(base) + offset);
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (exception_code_out != nullptr)
            {
                *exception_code_out = GetExceptionCode();
            }
            return 0;
        }
    }

    static auto seh_probe_u32(void* base, std::uintptr_t offset, std::uint32_t* value_out, DWORD* exception_code_out)
        -> int
    {
        __try
        {
            *value_out = *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(base) + offset);
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (exception_code_out != nullptr)
            {
                *exception_code_out = GetExceptionCode();
            }
            return 0;
        }
    }

    auto log_asset_manager_probe(void* asset_manager) -> void;

    auto register_mod_paths(void* asset_manager) -> void;

    auto register_paths_via_list(void* asset_manager, const std::vector<std::int64_t>& keys, DWORD* exception_code_out)
        -> bool
    {
        if (keys.empty())
        {
            return false;
        }

        SoftPathKeyArray array{};
        array.data = const_cast<std::int64_t*>(keys.data());
        array.count = static_cast<std::int32_t>(keys.size());
        array.capacity = array.count;

        __try
        {
            if (g_original_register_list != nullptr)
            {
                g_original_register_list(asset_manager, &array);
            }
            else
            {
                game_fn<RegisterSoftObjectPathListFn>(RVA_RegisterSoftObjectPathList)(asset_manager, &array);
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (exception_code_out != nullptr)
            {
                *exception_code_out = GetExceptionCode();
            }
            return false;
        }
    }

    void hooked_register_soft_object_path_list(void* asset_manager, void* path_array)
    {
        if (g_original_register_list == nullptr)
        {
            return;
        }

        const uintptr_t return_rva = caller_rva();

        log_line(L"[AbramsAssetRegistrar] register_list hook fired (game caller)");
        log_ptr(L"hook asset_manager", asset_manager);

        g_original_register_list(asset_manager, path_array);

        g_hook_asset_manager.store(asset_manager);
        g_game_bulk_register_complete.store(true);
        log_line(L"[AbramsAssetRegistrar] game bulk register_list complete");

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
            return;
        }

        void* register_trampoline = nullptr;
        void* register_target = reinterpret_cast<void*>(game_base() + RVA_RegisterSoftObjectPathList);
        if (install_hook(register_target, reinterpret_cast<void*>(&hooked_register_soft_object_path_list),
                         kRegisterListStolenBytes, &register_trampoline))
        {
            g_original_register_list = reinterpret_cast<RegisterSoftObjectPathListFn>(register_trampoline);
            log_line(L"[AbramsAssetRegistrar] Hook installed on register_soft_object_path_list");
        }
        else
        {
            log_line(L"[AbramsAssetRegistrar] Failed to hook register_soft_object_path_list");
        }
    }

    static auto seh_read_qword(void* base, std::uintptr_t offset, std::uint64_t* value_out,
                               DWORD* exception_code_out) -> int
    {
        __try
        {
            *value_out = *reinterpret_cast<std::uint64_t*>(reinterpret_cast<std::uint8_t*>(base) + offset);
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (exception_code_out != nullptr)
            {
                *exception_code_out = GetExceptionCode();
            }
            return 0;
        }
    }

    auto read_inner_from_wrapper(void* wrapper, void** inner_out) -> bool
    {
        if (wrapper == nullptr || inner_out == nullptr)
        {
            return false;
        }

        DWORD exception_code = 0;
        return seh_read_pointer_at(wrapper, kWrapperInnerOffset, inner_out, &exception_code) && *inner_out != nullptr;
    }

    static auto seh_call_game_instance(void** instance_out, DWORD* exception_code_out) -> int
    {
        __try
        {
            *instance_out = game_fn<GameInstanceAccessorFn>(RVA_GameInstanceAccessor)();
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (exception_code_out != nullptr)
            {
                *exception_code_out = GetExceptionCode();
            }
            return 0;
        }
    }

    struct ManagerRegistryState
    {
        bool readable{false};
        std::uint32_t pending_count{0};
        std::uint32_t pending_hash_capacity{0};
        void* committed_map_data{nullptr};
        std::uint32_t committed_count{0};
    };

    auto get_manager_from_game_instance() -> void*
    {
        void* instance = nullptr;
        void* manager = nullptr;
        DWORD exception_code = 0;
        if (!seh_call_game_instance(&instance, &exception_code) || instance == nullptr)
        {
            return nullptr;
        }

        if (!seh_read_pointer_at(instance, kGameInstanceManagerOffset, &manager, &exception_code))
        {
            return nullptr;
        }

        return manager;
    }

    auto probe_manager_registry_state(void* manager, ManagerRegistryState* state_out) -> bool
    {
        if (manager == nullptr || state_out == nullptr)
        {
            return false;
        }

        DWORD exception_code = 0;
        state_out->readable =
            seh_probe_u32(manager, kPendingSetCountOffset, &state_out->pending_count, &exception_code) &&
            seh_probe_u32(manager, kPendingHashCapacityOffset, &state_out->pending_hash_capacity, &exception_code) &&
            seh_read_pointer_at(manager, kCommittedMapDataOffset, &state_out->committed_map_data, &exception_code) &&
            seh_probe_u32(manager, kCommittedCountOffset, &state_out->committed_count, &exception_code);
        return state_out->readable;
    }

    auto is_native_registry_initialized(const ManagerRegistryState& state) -> bool
    {
        // game_instance+0x220 can show pending/committed > 0 while capacity@+0x3E4 stays 0.
        if (state.pending_count > 0 || state.committed_count > 0)
        {
            return true;
        }

        // Empty but hash buckets allocated (wrapper+0x10 inner pattern before first add).
        return state.pending_hash_capacity == kExpectedPendingHashCapacity;
    }

    auto mark_registry_ready_if_initialized(void* manager) -> void
    {
        ManagerRegistryState state{};
        if (manager == nullptr || !probe_manager_registry_state(manager, &state))
        {
            return;
        }

        if (!is_native_registry_initialized(state))
        {
            return;
        }

        g_game_bulk_register_complete.store(true);
        if (g_hook_asset_manager.load() == nullptr)
        {
            g_hook_asset_manager.store(manager);
            log_line(L"[AbramsAssetRegistrar] registry already initialized (hook missed startup)");
            log_ptr(L"recovered native manager", manager);
        }
    }

    auto is_game_registry_ready(void* wrapper) -> bool
    {
        if (g_game_bulk_register_complete.load())
        {
            return true;
        }

        void* manager = get_manager_from_game_instance();
        void* inner = nullptr;
        if (manager == nullptr && !read_inner_from_wrapper(wrapper, &inner))
        {
            return false;
        }
        if (manager == nullptr)
        {
            manager = inner;
        }

        ManagerRegistryState state{};
        if (!probe_manager_registry_state(manager, &state))
        {
            return false;
        }

        if (is_native_registry_initialized(state))
        {
            mark_registry_ready_if_initialized(manager);
            return true;
        }

        return false;
    }

    auto log_manager_registry_state(const wchar_t* label, void* manager) -> void
    {
        ManagerRegistryState state{};
        if (!probe_manager_registry_state(manager, &state))
        {
            log_line(std::wstring(L"[AbramsAssetRegistrar] ") + label + L": registry state unreadable");
            return;
        }

        wchar_t msg[256]{};
        swprintf_s(msg,
                   L"[AbramsAssetRegistrar] %s pending=%u capacity=0x%X committed=%u table=0x%llX",
                   label, state.pending_count, state.pending_hash_capacity, state.committed_count,
                   static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(state.committed_map_data)));
        log_line(msg);
    }

    auto resolve_wrapper_manager(void* lua_uobject_hint) -> void*
    {
        log_ptr(L"Lua UObject hint (timing only)", lua_uobject_hint);

        void* wrapper = nullptr;
        DWORD get_exception = 0;
        if (!seh_call_get_abrams_asset_manager(&wrapper, &get_exception))
        {
            wchar_t msg[96]{};
            swprintf_s(msg, L"[AbramsAssetRegistrar] GetAbramsAssetManager SEH=0x%08X", get_exception);
            log_line(msg);
            return nullptr;
        }

        if (wrapper == nullptr)
        {
            log_line(L"[AbramsAssetRegistrar] GetAbramsAssetManager returned null");
            return nullptr;
        }

        log_ptr(L"GetAbramsAssetManager wrapper", wrapper);

        if (lua_uobject_hint == wrapper)
        {
            log_line(L"[AbramsAssetRegistrar] Lua UObject matches GetAbrams wrapper");
        }
        else if (lua_uobject_hint != nullptr)
        {
            log_line(L"[AbramsAssetRegistrar] Lua UObject differs from GetAbrams wrapper");
        }

        void* inner = nullptr;
        DWORD read_exception = 0;
        if (seh_read_pointer_at(wrapper, kWrapperInnerOffset, &inner, &read_exception) && inner != nullptr)
        {
            log_ptr(L"wrapper+0x10 inner", inner);
            if (void* from_game = get_manager_from_game_instance(); from_game != nullptr && from_game != inner)
            {
                log_ptr(L"game_instance+0x220 (registration target)", from_game);
                log_line(L"[AbramsAssetRegistrar] WARNING: inner != game_instance+0x220; use game_instance for native ops");
            }
            if (lua_uobject_hint == inner)
            {
                log_line(L"[AbramsAssetRegistrar] Lua UObject matches wrapper+0x10 inner");
            }
        }
        else if (read_exception != 0)
        {
            wchar_t msg[96]{};
            swprintf_s(msg, L"[AbramsAssetRegistrar] failed to read wrapper+0x10 SEH=0x%08X", read_exception);
            log_line(msg);
        }

        log_line(L"[AbramsAssetRegistrar] using wrapper+0x10 inner for native registration");
        log_asset_manager_probe(inner != nullptr ? inner : wrapper);
        if (inner != nullptr && inner != wrapper)
        {
            log_line(L"[AbramsAssetRegistrar] wrapper probe (debug only)");
            log_asset_manager_probe(wrapper);
        }

        g_dump_wrapper = wrapper;
        g_dump_inner = inner;
        return inner != nullptr ? inner : wrapper;
    }

    auto resolve_registration_manager(void* lua_uobject_hint) -> void*
    {
        if (void* from_hook = g_hook_asset_manager.load(); from_hook != nullptr)
        {
            log_line(L"[AbramsAssetRegistrar] asset manager source=register_list hook");
            log_ptr(L"hooked native manager", from_hook);
            return from_hook;
        }

        if (void* from_game = get_manager_from_game_instance(); from_game != nullptr)
        {
            log_line(L"[AbramsAssetRegistrar] asset manager source=game_instance+0x220");
            log_ptr(L"game_instance native manager", from_game);
            return from_game;
        }

        return resolve_wrapper_manager(lua_uobject_hint);
    }

    auto log_asset_manager_probe(void* asset_manager) -> void
    {
        if (asset_manager == nullptr)
        {
            return;
        }

        struct OffsetLabel
        {
            std::uintptr_t offset;
            const wchar_t* label;
        };

        constexpr OffsetLabel probes[] = {
            {0x3B8, L"count@+0x3B8"},
            {0x3E4, L"capacity@+0x3E4"},
            {0x400, L"table@+0x400"},
            {0x450, L"commit@+0x450"},
        };

        for (const auto& probe : probes)
        {
            std::uint32_t value = 0;
            DWORD exception_code = 0;
            if (seh_probe_u32(asset_manager, probe.offset, &value, &exception_code))
            {
                wchar_t msg[128]{};
                swprintf_s(msg, L"[AbramsAssetRegistrar] probe %s = 0x%08X", probe.label, value);
                log_line(msg);
            }
            else
            {
                wchar_t msg[128]{};
                swprintf_s(msg, L"[AbramsAssetRegistrar] probe %s unreadable seh=0x%08X", probe.label, exception_code);
                log_line(msg);
            }
        }
    }

    auto log_soft_path_qwords(const wchar_t* label, const std::uint64_t* soft_path) -> void
    {
        wchar_t buf[192]{};
        swprintf_s(buf, L"[AbramsAssetRegistrar] %s soft_path=[0x%llX, 0x%llX, 0x%llX, 0x%llX]", label,
                   static_cast<unsigned long long>(soft_path[0]), static_cast<unsigned long long>(soft_path[1]),
                   static_cast<unsigned long long>(soft_path[2]), static_cast<unsigned long long>(soft_path[3]));
        log_line(buf);
    }

    static auto seh_build_soft_path(std::uint64_t* out, const wchar_t* object_path, std::uint32_t path_length,
                                    DWORD* exception_code_out, std::uint32_t* subobject_index_out,
                                    int* used_build_string_out) -> int
    {
        __try
        {
            auto build_key = game_fn<BuildSoftPathKeyFn>(RVA_BuildSoftPathKey);
            auto build_string = game_fn<BuildSoftPathFromStringFn>(RVA_BuildSoftPathFromString);

            for (std::uint32_t attempt = 0; attempt < 2; ++attempt)
            {
                const auto subobject_index = attempt == 0 ? 1u : 0u;
                memset(out, 0, sizeof(std::uint64_t) * 4);
                build_key(out, path_length, object_path, subobject_index);
                if (out[0] != 0)
                {
                    *subobject_index_out = subobject_index;
                    *used_build_string_out = 0;
                    return 1;
                }

                memset(out, 0, sizeof(std::uint64_t) * 4);
                build_string(out, object_path, subobject_index);
                if (out[0] != 0)
                {
                    *subobject_index_out = subobject_index;
                    *used_build_string_out = 1;
                    return 1;
                }
            }

            return 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (exception_code_out != nullptr)
            {
                *exception_code_out = GetExceptionCode();
            }
            return -1;
        }
    }

    static auto seh_add_soft_path(void* asset_manager, std::uint64_t* soft_path, DWORD* exception_code_out) -> int
    {
        __try
        {
            game_fn<AddSoftObjectPathFn>(RVA_AddSoftObjectPath)(asset_manager, soft_path);
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (exception_code_out != nullptr)
            {
                *exception_code_out = GetExceptionCode();
            }
            return 0;
        }
    }

    static auto seh_commit_pending_paths(void* asset_manager, std::uint8_t* commit_before_out,
                                         std::uint8_t* commit_after_out, DWORD* exception_code_out) -> int
    {
        __try
        {
            auto* const commit_flag = reinterpret_cast<std::uint8_t*>(reinterpret_cast<std::uintptr_t>(asset_manager) +
                                                                    kAbramsAssetManagerCommitFlagOffset);
            *commit_before_out = *commit_flag;
            if (*commit_flag != 1)
            {
                *commit_flag = 1;
            }

            game_fn<CommitPendingSoftPathsFn>(RVA_CommitPendingSoftPaths)(asset_manager);
            *commit_after_out = *commit_flag;
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (exception_code_out != nullptr)
            {
                *exception_code_out = GetExceptionCode();
            }
            return 0;
        }
    }

    struct BuildSoftPathResult
    {
        bool ok{false};
        DWORD exception_code{0};
        const wchar_t* method{L"none"};
        std::uint32_t subobject_index{0};
    };

    auto build_soft_path_safe(std::uint64_t* out, const wchar_t* object_path, BuildSoftPathResult& result) -> bool
    {
        const auto path_length = static_cast<std::uint32_t>(wcslen(object_path));
        if (path_length == 0)
        {
            result.method = L"empty_path";
            return false;
        }

        std::uint32_t subobject_index = 0;
        int used_build_string = 0;
        const int build_rc =
            seh_build_soft_path(out, object_path, path_length, &result.exception_code, &subobject_index, &used_build_string);

        if (build_rc < 0)
        {
            result.method = L"seh_exception";
            return false;
        }
        if (build_rc == 0)
        {
            result.method = L"no_key_built";
            return false;
        }

        result.ok = true;
        result.subobject_index = subobject_index;
        result.method = used_build_string != 0 ? L"BuildSoftPathFromString" : L"BuildSoftPathKey";
        return true;
    }

    struct AddSoftPathResult
    {
        bool ok{false};
        DWORD exception_code{0};
    };

    auto add_soft_path_safe(void* asset_manager, std::uint64_t* soft_path, AddSoftPathResult& result) -> bool
    {
        const int add_rc = seh_add_soft_path(asset_manager, soft_path, &result.exception_code);
        result.ok = add_rc != 0;
        return result.ok;
    }

    auto commit_pending_paths_safe(void* asset_manager, DWORD* exception_code_out) -> bool
    {
        std::uint8_t commit_before = 0;
        std::uint8_t commit_after = 0;
        const int commit_rc =
            seh_commit_pending_paths(asset_manager, &commit_before, &commit_after, exception_code_out);
        if (commit_rc == 0)
        {
            return false;
        }

        wchar_t commit_msg[96]{};
        swprintf_s(commit_msg, L"[AbramsAssetRegistrar] commit flag %u -> %u at manager+0x%llX",
                   static_cast<unsigned>(commit_before), static_cast<unsigned>(commit_after),
                   static_cast<unsigned long long>(kAbramsAssetManagerCommitFlagOffset));
        log_line(commit_msg);
        return true;
    }

    auto log_existing_registry_keys(void* asset_manager) -> void
    {
        log_manager_registry_state(L"registry state before mod register", asset_manager);
    }

    auto register_mod_paths(void* asset_manager) -> void
    {
        if (g_registered.load())
        {
            log_line(L"[AbramsAssetRegistrar] skip: already registered");
            return;
        }
        if (g_registering.load())
        {
            log_line(L"[AbramsAssetRegistrar] skip: registration in progress");
            return;
        }
        if (g_cached_paths.empty())
        {
            log_line(L"[AbramsAssetRegistrar] skip: no cached paths");
            return;
        }
        if (asset_manager == nullptr)
        {
            log_line(L"[AbramsAssetRegistrar] skip: null asset manager");
            return;
        }

        g_registering.store(true);

        log_ptr(L"asset manager for registration", asset_manager);

        wchar_t header[160]{};
        swprintf_s(header,
                   L"[AbramsAssetRegistrar] register_mod_paths thread=%lu caller=0x%llX return=0x%llX asset_manager=0x%llX",
                   GetCurrentThreadId(), static_cast<unsigned long long>(caller_rva()),
                   static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(_ReturnAddress())),
                   static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(asset_manager)));
        log_line(header);
        log_asset_manager_probe(asset_manager);
        log_existing_registry_keys(asset_manager);
        write_manager_memory_dump(asset_manager);

        std::vector<std::int64_t> built_keys;
        built_keys.reserve(g_cached_paths.size());

        for (std::size_t i = 0; i < g_cached_paths.size(); ++i)
        {
            const auto& path = g_cached_paths[i];
            log_line(L"[AbramsAssetRegistrar] path[" + std::to_wstring(i) + L"]: " + path);

            alignas(16) std::uint64_t soft_path[4]{};
            BuildSoftPathResult build_result{};
            if (!build_soft_path_safe(soft_path, path.c_str(), build_result))
            {
                wchar_t build_msg[256]{};
                swprintf_s(build_msg, L"[AbramsAssetRegistrar] build FAILED method=%s subobj=%u seh=0x%08X",
                           build_result.method, build_result.subobject_index, build_result.exception_code);
                log_line(build_msg);
                continue;
            }

            wchar_t build_msg[256]{};
            swprintf_s(build_msg, L"[AbramsAssetRegistrar] build OK method=%s subobj=%u",
                       build_result.method, build_result.subobject_index);
            log_line(build_msg);
            log_soft_path_qwords(L"built", soft_path);
            built_keys.push_back(static_cast<std::int64_t>(soft_path[0]));
        }

        std::size_t registered = 0;
        std::size_t build_failed = g_cached_paths.size() - built_keys.size();
        std::size_t add_failed = 0;

        if (!built_keys.empty())
        {
            DWORD list_exception = 0;
            if (register_paths_via_list(asset_manager, built_keys, &list_exception))
            {
                registered = built_keys.size();
                log_line(L"[AbramsAssetRegistrar] register_soft_object_path_list OK for " +
                         std::to_wstring(registered) + L" keys");
            }
            else
            {
                wchar_t list_fail[128]{};
                swprintf_s(list_fail, L"[AbramsAssetRegistrar] register_soft_object_path_list FAILED seh=0x%08X",
                           list_exception);
                log_line(list_fail);

                for (std::size_t i = 0; i < built_keys.size(); ++i)
                {
                    alignas(16) std::uint64_t soft_path[4]{};
                    soft_path[0] = static_cast<std::uint64_t>(built_keys[i]);

                    AddSoftPathResult add_result{};
                    if (add_soft_path_safe(asset_manager, soft_path, add_result))
                    {
                        ++registered;
                        log_line(L"[AbramsAssetRegistrar] add OK path[" + std::to_wstring(i) + L"]");
                    }
                    else
                    {
                        ++add_failed;
                        wchar_t add_msg[128]{};
                        swprintf_s(add_msg, L"[AbramsAssetRegistrar] add FAILED path[%llu] seh=0x%08X",
                                   static_cast<unsigned long long>(i), add_result.exception_code);
                        log_line(add_msg);
                    }
                }
            }
        }

        g_registering.store(false);

        log_line(L"[AbramsAssetRegistrar] Registered " + std::to_wstring(registered) + L" / " +
                 std::to_wstring(g_cached_paths.size()) + L" soft paths (" + std::to_wstring(build_failed) +
                 L" key build failures, " + std::to_wstring(add_failed) + L" add failures)");

        if (registered > 0)
        {
            DWORD commit_exception = 0;
            if (!commit_pending_paths_safe(asset_manager, &commit_exception))
            {
                wchar_t commit_fail[96]{};
                swprintf_s(commit_fail, L"[AbramsAssetRegistrar] commit_pending_soft_paths failed seh=0x%08X",
                           commit_exception);
                log_line(commit_fail);
            }
            g_registered.store(true);
        }
    }
} // namespace

extern "C"
{
    // lua_CFunction: abrams_register_paths(lua_uobject_address)
    // Returns 1 = success, 2 = not ready (game registry empty), 0 = failed.
    __declspec(dllexport) int abrams_register_paths(lua_State* L)
    {
        if (lua_gettop(L) < 1 || !lua_isinteger(L, 1))
        {
            log_line(L"[AbramsAssetRegistrar] abrams_register_paths: expected integer address argument");
            lua_pushinteger(L, kLuaResultFailed);
            return 1;
        }

        const lua_Integer addr = lua_tointeger(L, 1);
        if (addr == 0)
        {
            log_line(L"[AbramsAssetRegistrar] abrams_register_paths: null address");
            lua_pushinteger(L, kLuaResultFailed);
            return 1;
        }

        if (g_registered.load())
        {
            lua_pushinteger(L, kLuaResultSuccess);
            return 1;
        }

        void* lua_uobject = reinterpret_cast<void*>(static_cast<uintptr_t>(addr));

        void* wrapper = nullptr;
        DWORD get_exception = 0;
        if (!seh_call_get_abrams_asset_manager(&wrapper, &get_exception) || wrapper == nullptr)
        {
            log_line(L"[AbramsAssetRegistrar] GetAbramsAssetManager unavailable");
            lua_pushinteger(L, kLuaResultFailed);
            return 1;
        }

        if (!is_game_registry_ready(wrapper))
        {
            const int defer_count = g_defer_log_counter.fetch_add(1);
            if (defer_count == 0 || defer_count % 20 == 0)
            {
                void* inner = nullptr;
                read_inner_from_wrapper(wrapper, &inner);
                log_ptr(L"defer wrapper", wrapper);
                log_ptr(L"defer inner", inner);
                if (void* from_game = get_manager_from_game_instance(); from_game != nullptr)
                {
                    log_ptr(L"defer game_instance+0x220", from_game);
                    log_manager_registry_state(L"defer registry", from_game);
                }
                else if (inner != nullptr)
                {
                    log_manager_registry_state(L"defer registry", inner);
                }
                log_line(L"[AbramsAssetRegistrar] defer: waiting for startup register_soft_object_path_list "
                         L"(hook or pending@+0x3B8 > 0)");
                log_asset_manager_probe(inner != nullptr ? inner : wrapper);
            }
            lua_pushinteger(L, kLuaResultNotReady);
            return 1;
        }

        g_defer_log_counter.store(0);
        log_line(L"[AbramsAssetRegistrar] abrams_register_paths() called from Lua");
        void* asset_manager = resolve_registration_manager(lua_uobject);
        if (asset_manager == nullptr)
        {
            lua_pushinteger(L, kLuaResultFailed);
            return 1;
        }

        if (g_registered.load())
        {
            lua_pushinteger(L, kLuaResultSuccess);
            return 1;
        }

        log_line(L"[AbramsAssetRegistrar] game registry ready, registering paths");
        register_mod_paths(asset_manager);
        lua_pushinteger(L, g_registered.load() ? kLuaResultSuccess : kLuaResultFailed);
        return 1;
    }

    __declspec(dllexport) void* start_mod()
    {
        begin_log_session();
        log_line(L"[AbramsAssetRegistrar] start_mod");
        cache_paths();
        install_hooks();
        return nullptr;
    }

    __declspec(dllexport) void uninstall_mod(void*)
    {
    }
}
