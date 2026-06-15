# AbramsAssetRegistrar

UE4SS C++ mod that registers character widget **soft object paths** with `AbramsAssetManager` at runtime. The base game does this for `CP_010`–`CP_270` at startup; mod characters (e.g. `CP_280`) are skipped unless you register their paths here.

## Project layout

```
AbramsAssetRegistrar/
├── cpp/
│   ├── dllmain.cpp          # Mod source (hooks + registration logic)
│   └── CMakeLists.txt       # Used by the full RE-UE4SS CMake build only
├── dlls/
│   └── main.dll             # Build output (loaded by UE4SS)
├── paths.json               # Preferred path list (JSON array)
├── paths.txt                # Fallback path list (one path per line)
├── build_standalone.ps1     # Recommended build script
├── CMakeLists.txt           # Full RE-UE4SS source build (optional)
└── manifest.json            # UE4SS mod manifest (version 3)
```

Enable the mod in `ue4ss/Mods/mods.txt`:

```
AbramsAssetRegistrar : 1
```

---

## Prerequisites

| Requirement | Notes |
|-------------|-------|
| **Windows x64** | Matches the game's Win64 binary |
| **Visual Studio 2022 or 2026** | Desktop development with C++ workload |
| **CMake 3.22+** | Only for the optional full RE-UE4SS build |
| **Rust toolchain** | Only for the optional full RE-UE4SS build |

The standalone build uses `cl.exe` and `link.exe` from the Visual Studio Developer environment. It does **not** link against UE4SS headers or `UE4SS.dll`; the DLL exports `start_mod` / `uninstall_mod` and calls game functions via hard-coded RVAs.

---

## Build (recommended): standalone

Use this for normal development. It compiles `cpp/dllmain.cpp` into a plain DLL and copies it to `dlls/main.dll`.

### Steps

1. Open **PowerShell**.
2. Change to this mod folder:

   ```powershell
   cd "C:\Program Files (x86)\Steam\steamapps\common\Jujutsu Kaisen CC\Jujutsu Kaisen CC\Binaries\Win64\ue4ss\Mods\AbramsAssetRegistrar"
   ```

3. Run:

   ```powershell
   .\build_standalone.ps1
   ```

4. Confirm output:

   ```
   Built ...\AbramsAssetRegistrar\dlls\main.dll
   ```

### What the script does

- Enters the Visual Studio Developer shell (`Enter-VsDevShell`)
- Compiles `cpp/dllmain.cpp` with `/std:c++latest /EHsc /MD /O2`
- Links a DLL into `C:\jjkcc-build\abrams-standalone\` (short path to avoid Windows `MAX_PATH` issues)
- Copies the result to `dlls/main.dll`

### Customizing Visual Studio path

`build_standalone.ps1` is configured for **Visual Studio 2026 Community** at:

```
C:\Program Files\Microsoft Visual Studio\18\Community
```

If you use a different edition or version, edit the `Import-Module` and `Enter-VsDevShell` lines at the top of `build_standalone.ps1`. For Visual Studio 2022, the install path is typically:

```
C:\Program Files\Microsoft Visual Studio\2022\Community
```

You can also build manually from a **x64 Native Tools** or **Developer PowerShell** prompt:

```bat
cl.exe /nologo /std:c++latest /EHsc /MD /O2 /c cpp\dllmain.cpp /Fo"%TEMP%\dllmain.obj"
link.exe /nologo /DLL "%TEMP%\dllmain.obj" /OUT:dlls\main.dll
```

---

## Build (optional): full RE-UE4SS source

Only needed if you change UE4SS-linked APIs or want to build through the official mod CMake target. This pulls in the entire RE-UE4SS dependency tree (including Rust) and takes much longer.

### Setup

1. Clone [RE-UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) tag **v3.0.1** as a sibling of this folder:

   ```
   ue4ss/Mods/RE-UE4SS/
   ```

2. Initialize submodules:

   ```powershell
   cd ..\RE-UE4SS
   git submodule update --init --recursive
   ```

3. Configure and build from the mod folder. Use a **short build directory** (the game install path is long and can hit Windows path-length limits):

   ```powershell
   cd ..\AbramsAssetRegistrar

   cmake -B C:\jjkcc-build\abrams `
     -G "Visual Studio 18 2026" `
     -DRust_RESOLVE_RUSTUP_TOOLCHAINS=OFF `
     -DRust_COMPILER=C:/Users/<you>/.rustup/toolchains/stable-x86_64-pc-windows-msvc/bin/rustc.exe `
     -DRust_CARGO=C:/Users/<you>/.rustup/toolchains/stable-x86_64-pc-windows-msvc/bin/cargo.exe

   cmake --build C:\jjkcc-build\abrams --config Game__Shipping__Win64 --target AbramsAssetRegistrar
   ```

   For Visual Studio 2022, use `-G "Visual Studio 17 2022"` instead.

4. The CMake post-build step copies the DLL to `dlls/main.dll`. You can also copy manually from:

   ```
   C:\jjkcc-build\abrams\cpp\Game__Shipping__Win64\AbramsAssetRegistrar.dll
   ```

---

## When to rebuild

| Change | Rebuild needed? |
|--------|-----------------|
| Edit `paths.json` or `paths.txt` | No — restart the game |
| Edit `cpp/dllmain.cpp` (RVAs, hooks, logic) | Yes |
| Game patch changes executable layout | Yes — update RVAs in `dllmain.cpp` |

Path loading priority at runtime: `paths.json` if present, otherwise `paths.txt` (`#` lines are comments).

---

## Game EXE RVAs (update after patches)

These constants are defined at the top of `cpp/dllmain.cpp`:

| Symbol | RVA |
|--------|-----|
| `GetAbramsAssetManager` | `0xFFDD20` |
| `RegisterSoftObjectPathList` | `0x14894E0` |
| `AddSoftObjectPath` | `0x14895D0` |
| `CommitPendingSoftPaths` | `0x14BF520` |
| `BuildSoftPathKey` | `0x18F8230` |
| `BuildSoftPathFromString` | `0x18F8530` |

After a game update, re-resolve these in Ghidra/IDA and update `dllmain.cpp`, then rebuild.

---

## Verifying the build

1. Launch the game with UE4SS enabled.
2. Check `register.log` in this mod folder for lines like:
   ```
   [AbramsAssetRegistrar] start_mod
   [AbramsAssetRegistrar] Hook installed on register_soft_object_path_list
   ```
3. Confirm character select / HUD icons load for the paths you registered.

---

## Adding paths (no rebuild)

Edit `paths.json` (preferred) or `paths.txt`, then restart the game.

**paths.json** — JSON array of strings:

```json
[
  "/Game/Mods/SampleNewTextures/T_UI_Test.T_UI_Test",
  "/Game/Widgets/Characters/CP_280/TexturesNonAtlas/T_UI_Character_CharacterSelect_Icon_Square_CP_280_00.T_UI_Character_CharacterSelect_Icon_Square_CP_280_00"
]
```

**paths.txt** — one full object path per line; `#` starts a comment.

Paths can be under `/Game/Widgets/Characters/...` or `/Game/Mods/...` as long as the mod loader mounts the assets.

---

## What this mod does not fix

- 3D character model preview on the select screen (requires character blueprint + capture data)
