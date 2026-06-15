# Builds AbramsAssetRegistrar as a plain DLL (no UE4SS linking).
# Links a minimal Lua 5.4 core so exports can be proper lua_CFunctions.
$ErrorActionPreference = "Stop"

$modRoot = $PSScriptRoot
$outDll = Join-Path $modRoot "dlls\main.dll"
$buildDir = "C:\jjkcc-build\abrams-standalone"
$luaRoot = Join-Path $modRoot "..\RE-UE4SS\deps\first\LuaRaw"
$luaInc = Join-Path $luaRoot "include"
$luaSrc = Join-Path $luaRoot "src"

Import-Module "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath "C:\Program Files\Microsoft Visual Studio\18\Community" -SkipAutomaticLocation -DevCmdArguments "-arch=amd64"

New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path $outDll) | Out-Null

$luaCore = @(
    "lapi", "lcode", "lctype", "ldebug", "ldo", "ldump", "lfunc", "lgc", "llex", "lmem",
    "lobject", "lopcodes", "lparser", "lstate", "lstring", "ltable", "ltm",
    "lundump", "lvm", "lzio", "luauser"
)

$objs = @()
foreach ($name in $luaCore) {
    $srcFile = Join-Path $luaSrc "$name.c"
    $objFile = Join-Path $buildDir "$name.obj"
    cl.exe /nologo /TC /O2 /MD /c $srcFile "/I$luaInc" "/Fo$objFile"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $objs += $objFile
}

$dllmainObj = Join-Path $buildDir "dllmain.obj"
$source = Join-Path $modRoot "cpp\dllmain.cpp"
cl.exe /nologo /std:c++latest /EHsc /MD /O2 /c $source "/I$luaInc" "/Fo$dllmainObj"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$objs += $dllmainObj

$linkOut = Join-Path $buildDir "AbramsAssetRegistrar.dll"
link.exe /nologo /DLL $objs "/OUT:$linkOut"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Copy-Item -Force $linkOut $outDll
Write-Host "Built $outDll"
