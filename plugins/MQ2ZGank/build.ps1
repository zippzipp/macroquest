# build.ps1 — rebuild ONLY MQ2ZGank, never MQ core.
#
# Safe to run while EverQuest is running: it links against the existing
# mq2main.lib and does NOT relink the locked core DLLs. Core is built only when
# you change MacroQuest itself.
#
# Typical loop while the game is up:
#   1) in-game:  /plugin MQ2ZGank unload     (releases the plugin DLL lock)
#   2) run this script
#   3) in-game:  /plugin MQ2ZGank load

$ErrorActionPreference = 'Stop'

$msbuild = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
    -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe |
    Select-Object -First 1

if (-not $msbuild) { throw "MSBuild not found via vswhere." }

$proj = Join-Path $PSScriptRoot 'MQ2ZGank.vcxproj'

& $msbuild $proj `
    /p:Configuration=Release `
    /p:Platform=x64 `
    /p:BuildProjectReferences=false `
    /m /v:minimal /nologo

$dll = Join-Path $PSScriptRoot '..\..\..\build\bin\release\plugins\MQ2ZGank.dll'
if (Test-Path $dll) {
    Write-Host "`nBuilt: $((Resolve-Path $dll).Path)" -ForegroundColor Green
    Write-Host "Reload in-game:  /plugin MQ2ZGank load" -ForegroundColor Cyan
}
