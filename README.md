# AbramsAssetRegistrar

Registers character widget **soft object paths** with `AbramsAssetManager` at runtime (Strategy A).

This is what the base game does for `CP_010`–`CP_270` at startup. Mod characters like `CP_280` are skipped unless you register their paths here.

## How it works

UE4SS loads `dlls/main.dll` automatically (C++ mod slot). This DLL:

1. Exports `start_mod` / `uninstall_mod` so UE4SS accepts it
2. Returns `nullptr` from `start_mod` — **no UE4SS headers or linking**
3. Spawns a thread that calls game RVAs directly and registers paths from `paths.json` / `paths.txt`

No Lua script required.

- Character select icons
- Battle HUD face icons
- Cut-ins / outgame portraits / sprites

Paths can be canonical (`/Game/Widgets/Characters/CP_280/...`) or live under `/Game/Mods/...` as long as the mod loader mounts them.

## What it does not fix

- 3D character model preview on select (needs character BP + capture data)

## Path list (no rebuild needed)

Edit **`paths.json`** or **`paths.txt`** in this mod folder, then restart the game. The DLL reads the file every launch.

- If `paths.json` exists, it is used.
- Otherwise `paths.txt` is used (one full object path per line; `#` comments allowed).

You only need to run `build_standalone.ps1` again if you change the C++ source (`cpp/dllmain.cpp`), not when adding or removing paths.

| File | Purpose |
|------|---------|
| `paths.json` | JSON array of path strings (preferred if present) |
| `paths.txt` | One full object path per line |
| `dlls/main.dll` | Built once; registers paths from the files above |

## Build (recommended)

From PowerShell, run:

```powershell
.\build_standalone.ps1
```

This compiles against the installed `UE4SS.dll` (no full RE-UE4SS source build). Output: `dlls/main.dll`.

Requires **Visual Studio 2022/2026** with C++ desktop tools.

## Build (full RE-UE4SS source)

Only needed if you change UE4SS-linked APIs. Requires Rust + VS, and a short build path (see `build_standalone.ps1` for the workaround used on this machine).

1. Clone [RE-UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) tag **v3.0.1** into `ue4ss/Mods/RE-UE4SS`
2. `git submodule update --init --recursive` inside that repo
3. Configure with Rust on PATH and build dir `C:\jjkcc-build\abrams` (avoids Windows path-length limits):

```bat
cmake -B C:\jjkcc-build\abrams -G "Visual Studio 18 2026" -DRust_RESOLVE_RUSTUP_TOOLCHAINS=OFF -DRust_COMPILER=C:/Users/<you>/.rustup/toolchains/stable-x86_64-pc-windows-msvc/bin/rustc.exe -DRust_CARGO=C:/Users/<you>/.rustup/toolchains/stable-x86_64-pc-windows-msvc/bin/cargo.exe
cmake --build C:\jjkcc-build\abrams --config Game__Shipping__Win64 --target AbramsAssetRegistrar
```

## Game EXE RVAs (update after patches)

| Symbol | RVA |
|--------|-----|
| `GetAbramsAssetManager` | `0xFFDD20` |
| `AbramsAssetManager_add_soft_object_path` | `0x14895D0` |
| `BuildSoftPathKey` (FSoftObjectPath from string) | `0x18F8230` |

## Adding paths

Edit `paths.txt`. Examples:

```
/Game/Mods/SampleNewTextures/T_UI_Test.T_UI_Test
/Game/Widgets/Characters/CP_280/TexturesNonAtlas/T_UI_Character_CharacterSelect_Icon_Square_CP_280_00.T_UI_Character_CharacterSelect_Icon_Square_CP_280_00
```

Lines starting with `#` are ignored.
