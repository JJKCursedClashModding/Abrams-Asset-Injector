# AbramsAssetManager — Ghidra RE Notes

Reverse-engineered from `Jujutsu Kaisen CC.exe` (image base `0x140000000`, PE x64).  
Engine config: `AssetManagerClassName=/Script/Abrams.AbramsAssetManager` in `DefaultEngine.ini`.

Annotations (names, prototypes, plate comments) were applied in Ghidra via MCP and the program was saved.

---

## Architecture overview

```
Game singleton (DAT_14662ed20)
  └── +0x338  UObject* AbramsAssetManager wrapper   ← GetAbramsAssetManager()
        └── +0x10  native FAbramsAssetManager*      ← actual registry (use this for add/commit)

Startup path:
  AbramsStartupStateMachine_load_character_assets (case 1)
    → builds TArray<uint64> soft path keys (CP_010 … CP_270 widget textures)
    → AbramsAssetManager_register_soft_object_path_list(manager, &array)
         → AbramsAssetManager_add_soft_object_path (per key)

Runtime resolve:
  AbramsAssetManager_resolve_soft_object_path
    → AbramsAssetManager_lookup_asset_by_fname (committed TMap at manager+0x400)
```

Mod characters (e.g. `CP_280`) are **not** in the startup list; they must be registered manually (see `AbramsAssetRegistrar`).

---

## Function table (RVA = VA − 0x140000000)

| Ghidra name | VA | RVA | Role |
|-------------|-----|-----|------|
| `GetAbramsAssetManager` | `0x140FFDD20` | `0xFFDD20` | Return UObject wrapper from singleton `+0x338` |
| `AbramsAssetManager_register_soft_object_path_list` | `0x1414894E0` | `0x14894E0` | Bulk register `TArray<uint64>` keys |
| `AbramsAssetManager_add_soft_object_path` | `0x1414895D0` | `0x14895D0` | Insert one key into pending registry |
| `AbramsAssetManager_commit_pending_soft_paths` | `0x1414BF520` | `0x14BF520` | Flush pending → committed when `+0x450==1` |
| `AbramsAssetManager_validate_soft_path_list` | `0x140FFE520` | `0xFFE520` | All entries in a TSet loaded? (startup gate) |
| `AbramsAssetManager_is_soft_path_unresolved` | `0x140FFE480` | `0xFFE480` | Single-key unresolved check |
| `AbramsAssetManager_resolve_soft_object_path` | `0x143319AF0` | `0x3319AF0` | Key → `FSoftObjectPtr` / UObject* |
| `AbramsAssetManager_lookup_asset_by_fname` | `0x143318B30` | `0x3318B30` | Hash lookup in committed map |
| `AbramsAssetManager_register_dynamic_soft_paths` | `0x14164BB30` | `0x164BB30` | DLC/dynamic table → add_soft_object_path |
| `BuildSoftPathKey` | `0x1418F8230` | `0x18F8230` | `(path, len, subobj)` → 8-byte key |
| `BuildSoftPathFromString` | `0x1418F8530` | `0x18F8530` | `(wstring, subobj)` → 8-byte key |
| `AbramsStartupStateMachine_load_character_assets` | `0x1413A0F00` | `0x13A0F00` | Startup FSM; case 1 registers base roster |

**Singleton accessor:** `FUN_141017BD0()` returns the Abrams game instance; `*(instance + 0x220)` is the **native** manager pointer used by startup registration (same target as `wrapper+0x10` once initialized).

---

## Native object layout (inner manager at `wrapper+0x10`)

Offsets inferred from `add_soft_object_path`, `commit_pending_soft_paths`, and runtime dumps.

| Offset | Field | Notes |
|--------|-------|-------|
| `+0x380` | `TArray<void*>` | Callback / listener objects; iterated during commit |
| `+0x3A0` | `TArray<LoadHandle*>` | Pending async load handles (8 bytes each) |
| `+0x3A8` | `int32` pending_handle_count | |
| `+0x3AC` | `int32` pending_handle_capacity | |
| `+0x3B0` | `TSet` pending soft paths | 0x18-byte entries: `{uint64 key, ptr payload, int32 next}` |
| `+0x3B8` | `int32` pending_set_count | Hash occupancy / size field used in add |
| `+0x3E4` | `int32` hash_capacity | `0x80` when table allocated but empty |
| `+0x3F0` | `int32*` hash_slots_alloc | Optional heap backing for hash index |
| `+0x3F8` | `int32` hash_mask | `capacity - 1` |
| `+0x400` | `TMap` committed registry | 0x18-byte entries; **must be initialized before add** |
| `+0x408` | `int32` committed_count | |
| `+0x434` | `int32` committed_capacity | |
| `+0x438` | hash index base (inline) | |
| `+0x440` | `int32*` hash_slots_alloc | |
| `+0x448` | `int32` hash_mask | |
| `+0x450` | `uint8` commit_pending_flag | `1` = dirty, cleared by `commit_pending_soft_paths` |
| `+0x544` | `void*` dynamic path source | Read by `register_dynamic_soft_paths` |
| `+0x550` | `TArray<uint64>` cached dynamic keys | |

**UObject wrapper** (`GetAbramsAssetManager` return value): native pointer at `+0x10`. Do not call `add_soft_object_path` on the wrapper itself — hash table at `wrapper+0x400` stays zero.

---

## Per-function behaviour

### `GetAbramsAssetManager` — `0x140FFDD20`

```c
void* __fastcall GetAbramsAssetManager(void);
```

- Loads `DAT_14662ed20` (game singleton).
- If `*(singleton + 0x338)` non-null, validates UObject class via `FUN_140e8a090` (IsA check).
- Returns wrapper pointer or `NULL`.
- ~100 call sites (UI, commit, validate, startup).

### `AbramsAssetManager_register_soft_object_path_list` — `0x1414894E0`

```c
void __fastcall AbramsAssetManager_register_soft_object_path_list(
    void* asset_manager, longlong* soft_path_array);
```

- `soft_path_array[0]` = pointer to first `uint64` key; `soft_path_array[1]` = count.
- Deduplicates into a local `TArray<uint64>` while calling `add_soft_object_path` for each new key.
- Callers include `AbramsStartupStateMachine_load_character_assets`, DLC loaders (`0x141591B39`, `0x1413CDB83`).

### `AbramsAssetManager_add_soft_object_path` — `0x1414895D0`

```c
void __fastcall AbramsAssetManager_add_soft_object_path(
    void* asset_manager, ulonglong* soft_path_key);
```

Algorithm (decompiled):

1. Skip if `*soft_path_key == 0`.
2. Bump ref-count in side table at `manager+0x400` via `FUN_1412a9b40`.
3. If key already in pending `TSet` at `+0x3B0`, return early.
4. Allocate load-state object (`FUN_140ff69d0`), init vtable `PTR_FUN_144d81008`.
5. `FUN_141489470` binds key; `FUN_14146f660` wires async callback.
6. Insert into `TSet` at `+0x3B0` via `FUN_1408338c0`.
7. Append handle to `TArray` at `+0x3A0`.

**Mod note:** calling this before `+0x400` hash table is allocated causes AV (`0xC0000005`) — wait until `capacity@+0x3E4 == 0x80` and game has run `register_soft_object_path_list` at least once.

### `AbramsAssetManager_commit_pending_soft_paths` — `0x1414BF520`

```c
void __fastcall AbramsAssetManager_commit_pending_soft_paths(void* asset_manager);
```

- No-op unless `*(uint8*)(manager+0x450) == 1`.
- Iterates pending `TSet` at `+0x3B0` using UE bit-iterator pattern.
- For each entry not yet in committed map (`+0x400`), if asset loaded (`FUN_1412d5860`), resolves UObject via `GetAbramsAssetManager` → vtable `+0x318`, inserts into committed map.
- On full success, runs listeners in `TArray` at `+0x380` and clears `+0x450`.

### `AbramsAssetManager_resolve_soft_object_path` — `0x143319AF0`

```c
ulonglong* __fastcall AbramsAssetManager_resolve_soft_object_path(
    void* asset_manager, ulonglong* out, ulonglong soft_path_key,
    char load_sync, longlong load_context);
```

- Looks up asset record via `lookup_asset_by_fname`.
- Writes `FSoftObjectPtr` to `out` (object ptr + ref-counted state).
- Chooses asset bundle slot at record `+0x50` or `+0x70` based on `load_sync`.
- **Not** invoked through `ProcessEvent` — direct native call only.

### `AbramsAssetManager_lookup_asset_by_fname` — `0x143318B30`

```c
longlong* __fastcall AbramsAssetManager_lookup_asset_by_fname(
    longlong* asset_manager, ulonglong* soft_path_key, char allow_redirect);
```

- Probes committed `TMap` at `asset_manager+0x470` (0x20-byte values).
- Hash: `FUN_1418fe510(key_low) + key_high`, masked by `+0x4B8`.
- If miss and `allow_redirect`, calls UObject vtable `+0x450` to resolve redirect path and retries recursively.

### `BuildSoftPathKey` / `BuildSoftPathFromString`

```c
ulonglong* __fastcall BuildSoftPathKey(
    ulonglong* out, int path_length, wchar_t* path, int subobject_index);

ulonglong* __fastcall BuildSoftPathFromString(
    ulonglong* out, ushort* path, int subobject_index);
```

- Pack `FSoftObjectPath` into 8 bytes: path name index/hash + subobject index (high dword).
- `BuildSoftPathFromString` computes length from null-terminated UTF-16 string.
- `subobject_index` typically `1` for widget textures (`_C` suffix paths use class subobject).

### `AbramsAssetManager_validate_soft_path_list` — `0x140FFE520`

```c
ulonglong __fastcall AbramsAssetManager_validate_soft_path_list(
    void* asset_manager, longlong* soft_path_set);
```

- Iterates UE `TSet` (same bit-iterator as commit).
- Returns `1` if every entry has non-null payload **and** `FUN_1412d5860` reports loaded.
- Used heavily in startup cases 2 and 5 to block progression until assets stream in.

### `AbramsAssetManager_register_dynamic_soft_paths` — `0x14164BB30`

- Reads dynamic character list at `startup_state+0x544`.
- For each entry, scans game data tables, builds key with `BuildSoftPathKey`, dedupes.
- Batch-calls `add_soft_object_path` on `FUN_141017BD0()+0x220`.
- Stores copy at `startup_state+0x550`.

---

## Startup integration

`AbramsStartupStateMachine_load_character_assets` (`0x1413A0F00`) state switch:

| Case | Action |
|------|--------|
| 0 | Init; preload |
| **1** | Build character widget soft paths; **`register_soft_object_path_list`** |
| **2** | **`validate_soft_path_list`** on multiple TSets — wait for load |
| 3–4 | Further asset bundling |
| **5** | Validate UI texture sets |
| 6–8 | World / match setup |

Case 1 is the path that registers `CP_010`–`CP_270` icons. Mod paths must hook the same pipeline **after** case 1 has initialized the hash table.

---

## Soft path key format

`uint64` passed to add/register functions:

```
  bits  0..31  : FName / path index (from BuildSoftPath*)
  bits 32..63  : subobject name index (usually 0; use 1 for `.AssetName` subobjects)
```

Example from mod log: `/Game/Mods/.../T_UI_Test.T_UI_Test` → `[0x427C93, 0, 0, 0]` (only low dword set).

---

## Hook / mod guidance

1. **Resolve manager:** `GetAbramsAssetManager()` → `*(wrapper+0x10)` for native ops.
2. **Timing:** defer registration until `probe capacity@+0x3E4 == 0x80` or hook `register_soft_object_path_list` (RVA `0x14894E0`).
3. **Build keys:** `BuildSoftPathFromString(buf, L"/Game/.../Asset.Asset", 1)`.
4. **Register:** `register_soft_object_path_list(mgr, &{data,count})` or single `add_soft_object_path(mgr, &key)`.
5. **Optional:** `commit_pending_soft_paths(mgr)` if `+0x450` is set.

---

## Related symbols (not renamed in this pass)

| Address | Role |
|---------|------|
| `FUN_141017BD0` | Game instance / Abrams singleton accessor |
| `FUN_1418fe510` | FName / path hash for TMap keys |
| `FUN_1412a9b40` | TMap find-or-add at `manager+0x400` |
| `FUN_1408338c0` | TSet emplace |
| `FUN_1412d5860` | Is soft-path asset loaded? (calls `is_soft_path_unresolved`) |
| `FUN_141489470` | Bind key to pending load handle |
| `DAT_14662ed20` | Global game singleton pointer |

---

## Ghidra session

- Program: `Jujutsu Kaisen CC.exe`
- Image base: `0x140000000`
- String xref: `"AbramsAssetManager"` @ `0x1449D8A6A`
- Changes saved to Ghidra project after annotation pass
