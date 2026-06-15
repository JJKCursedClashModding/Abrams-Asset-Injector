# Builds AbramsAssetRegistrar as a plain DLL (no UE4SS linking).
$ErrorActionPreference = "Stop"

$modRoot = $PSScriptRoot
$outDll = Join-Path $modRoot "dlls\main.dll"
$buildDir = "C:\jjkcc-build\abrams-standalone"

Import-Module "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath "C:\Program Files\Microsoft Visual Studio\18\Community" -SkipAutomaticLocation -DevCmdArguments "-arch=amd64"

New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path $outDll) | Out-Null

$source = Join-Path $modRoot "cpp\dllmain.cpp"
$obj = Join-Path $buildDir "dllmain.obj"
$linkOut = Join-Path $buildDir "AbramsAssetRegistrar.dll"

cl.exe /nologo /std:c++latest /EHsc /MD /O2 /c $source "/Fo$obj"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

link.exe /nologo /DLL $obj "/OUT:$linkOut"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Copy-Item -Force $linkOut $outDll
Write-Host "Built $outDll"
