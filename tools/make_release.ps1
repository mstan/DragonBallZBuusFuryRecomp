<#
Build the DragonBallZBuusFuryRecomp Windows x64 release archive.

The archive contains the stripped executable, launcher box art and shared UI
assets, the built-in mod catalog, MinGW/SDL runtime DLLs, the toolchain-less
overlay compiler, and a short player README. It never contains a ROM, retail
GBA BIOS, save data, generated source, or developer configuration.

Usage:
  pwsh tools\make_release.ps1 -Version 0.0.1
#>
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$BuildDir = 'build-release',
    [string]$GbarecompRoot = '',
    [ValidateRange(1, 32)][int]$Jobs = 4
)

$ErrorActionPreference = 'Stop'

if ($Version -notmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$') {
    throw "Version must look like 0.0.1 (received '$Version')."
}

$mingwBin = 'C:\msys64\mingw64\bin'
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = (Resolve-Path (Join-Path $scriptDir '..')).Path
if (-not $GbarecompRoot) { $GbarecompRoot = Join-Path $root 'gbarecomp' }
$engine = (Resolve-Path $GbarecompRoot).Path
$build = Join-Path $root $BuildDir
$out = Join-Path $root 'release-stage'
$target = 'DragonBallZBuusFuryRecomp'
$gameTitle = "Dragon Ball Z: Buu's Fury"
$expectedSha1 = 'e65738e9d67688309f09811a54f495523ec9aada'
$stageName = "$target-windows-x64-v$Version"
$stage = Join-Path $out $stageName
$zip = Join-Path $out "$stageName.zip"
$dlls = @('SDL2.dll', 'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')

foreach ($required in @(
    (Join-Path $mingwBin 'cc.exe'),
    (Join-Path $mingwBin 'c++.exe'),
    (Join-Path $mingwBin 'ninja.exe'),
    (Join-Path $mingwBin 'strip.exe'),
    (Join-Path $engine 'CMakeLists.txt'),
    (Join-Path $engine 'tools\fetch_tcc.ps1'),
    (Join-Path $engine 'src\runtime\generated_bios\bios_recompiled.cpp'),
    (Join-Path $engine 'src\runtime\generated_bios\bios_recompiled.h'),
    (Join-Path $root 'recomp\launcher\boxart.tga')
)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required release dependency is missing: $required"
    }
}

$generated = @(Get-ChildItem -LiteralPath (Join-Path $root 'generated') `
    -Filter 'recompiled_*.cpp' -File -ErrorAction SilentlyContinue)
if ($generated.Count -eq 0) {
    throw 'Generated guest shards are missing. Supply the verified ROM and run tools\regen.ps1 first.'
}

$env:PATH = "$mingwBin;$env:PATH"
New-Item -ItemType Directory -Force -Path $out | Out-Null

& cmake -S $root -B $build -G Ninja `
    -DCMAKE_C_COMPILER="$mingwBin/cc.exe" `
    -DCMAKE_CXX_COMPILER="$mingwBin/c++.exe" `
    -DCMAKE_MAKE_PROGRAM="$mingwBin/ninja.exe" `
    -DCMAKE_BUILD_TYPE=Release `
    "-DCMAKE_CXX_FLAGS_RELEASE=-O1 -DNDEBUG" `
    -DGBARECOMP_ROOT="$engine" `
    -DGBARECOMP_BUILD_ORACLE=OFF `
    -DGBARECOMP_MINGW_PREFIX_UNIX='/c/msys64/mingw64' `
    -DSDL2_INCLUDE_DIR='C:/msys64/mingw64/include/SDL2' `
    -DSDL2_LIBRARY='C:/msys64/mingw64/lib/libSDL2.dll.a'
if ($LASTEXITCODE -ne 0) { throw "Release configure failed ($LASTEXITCODE)." }

& cmake --build $build --target $target --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw "Release build failed ($LASTEXITCODE)." }

$exe = Join-Path $build "$target.exe"
if (-not (Test-Path -LiteralPath $exe)) { throw "Expected executable is missing: $exe" }
& (Join-Path $mingwBin 'strip.exe') $exe
if ($LASTEXITCODE -ne 0) { throw "strip failed ($LASTEXITCODE)." }

if (Test-Path -LiteralPath $stage) {
    $resolvedStage = (Resolve-Path -LiteralPath $stage).Path
    $resolvedOut = (Resolve-Path -LiteralPath $out).Path
    if (-not $resolvedStage.StartsWith("$resolvedOut\", [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove stage outside release output: $resolvedStage"
    }
    Remove-Item -LiteralPath $resolvedStage -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item -LiteralPath $exe -Destination $stage
foreach ($dll in $dlls) {
    $source = Join-Path $mingwBin $dll
    if (-not (Test-Path -LiteralPath $source)) { throw "Runtime DLL is missing: $source" }
    Copy-Item -LiteralPath $source -Destination $stage
}

$assets = Join-Path $build 'assets'
if (-not (Test-Path -LiteralPath (Join-Path $assets 'img\boxart.tga'))) {
    throw "Launcher box art was not staged by the build: $assets"
}
Copy-Item -LiteralPath $assets -Destination $stage -Recurse

$mods = Join-Path $build 'mods'
if (-not (Test-Path -LiteralPath (Join-Path $mods 'packages'))) {
    throw "Preloaded mod catalog is missing: $mods"
}
Copy-Item -LiteralPath $mods -Destination $stage -Recurse

& (Join-Path $engine 'tools\fetch_tcc.ps1') `
    -Toolchain (Join-Path $stage 'overlay_toolchain') -EngineRoot $engine
if ($LASTEXITCODE -ne 0) { throw "Overlay toolchain staging failed ($LASTEXITCODE)." }

@"
# $gameTitle - GBA static recompilation (Windows x64)

This v$Version preview runs the USA retail game through gbarecomp with a
recomp-ui launcher and an optional Adaptive Widescreen mod.

## How to run

1. Extract this entire folder and keep its DLLs, assets, mods, and
   overlay_toolchain beside $target.exe.
2. Run $target.exe.
3. Select your legally obtained $gameTitle (USA) ROM.
   Expected SHA-1: $expectedSha1
4. Select your legally obtained retail gba_bios.bin under Settings > System.
5. Open Mods to opt into Adaptive Widescreen, then select PLAY.

Display, audio, input, and enhancement settings persist locally. Keyboard
defaults: arrows = D-pad, Z = A, X = B, Enter = Start, Backspace = Select,
and Tab = fast-forward. Shift+F1-F9 saves a state; F1-F9 loads one.

The ROM and retail GBA BIOS are not included. Supply your own dumps.

Project: https://github.com/mstan/$target
Engine: https://github.com/mstan/gbarecomp
"@ | Out-File -LiteralPath (Join-Path $stage 'README.md') -Encoding utf8

$forbidden = Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object {
    $relative = $_.FullName.Substring($stage.Length + 1)
    $_.Extension -in @('.gba', '.bin', '.bios', '.sav', '.srm', '.fla',
        '.flash', '.state', '.json', '.log', '.apk') -or
    $relative -match '(^|\\)(generated|recomp_cache)(\\|$)' -or
    ($_.Extension -in @('.cpp', '.c', '.h') -and
        $relative -notmatch '^overlay_toolchain\\') -or
    $_.Name -ieq 'gba_bios.bin' -or
    $_.Name -in @('game.toml', 'rom.cfg', 'bios.cfg', 'config.ini',
        'keybinds.ini', 'mods.ini', 'state.toml')
}
if ($forbidden) {
    throw "Forbidden release content detected: $($forbidden.FullName -join ', ')"
}

if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip

Write-Host "--- $stageName ---"
Get-ChildItem -LiteralPath $stage | Select-Object Name, Length | Out-Host
Get-Item -LiteralPath $zip | Select-Object Name, Length | Out-Host
Get-FileHash -LiteralPath $zip -Algorithm SHA256 |
    Select-Object Algorithm, Hash, Path | Out-Host
