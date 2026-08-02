param(
    [string]$Rom = (Join-Path $PSScriptRoot '..\roms\dragon_ball_z_buus_fury_usa.gba'),
    [string]$GbarecompRoot = (Join-Path $PSScriptRoot '..\gbarecomp'),
    [int]$MaxFunctions = 65536
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$romPath = (Resolve-Path $Rom).Path
$engine = (Resolve-Path $GbarecompRoot).Path
$tool = Join-Path $engine 'build\gba_recompile.exe'
if (-not (Test-Path -LiteralPath $tool)) {
    throw "Missing $tool. Build the engine's gba_recompile target first."
}

$actual = (Get-FileHash -LiteralPath $romPath -Algorithm SHA1).Hash.ToLowerInvariant()
$expected = 'e65738e9d67688309f09811a54f495523ec9aada'
if ($actual -ne $expected) {
    throw "Buu's Fury ROM SHA-1 mismatch: got $actual expected $expected"
}

& $tool --rom $romPath --config (Join-Path $root 'game.toml') `
    --out (Join-Path $root 'generated') --max-functions $MaxFunctions
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
