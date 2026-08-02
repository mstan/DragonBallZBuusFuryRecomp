param(
    [Parameter(Mandatory = $true)]
    [string]$State,
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build'),
    [int]$Frames = 6000,
    [int]$ViewWidth = 480
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$statePath = (Resolve-Path $State).Path
$exe = Join-Path (Resolve-Path $BuildDir).Path `
    'DragonBallZBuusFuryRecomp.exe'
if (-not (Test-Path -LiteralPath $exe)) {
    throw "Missing executable: $exe"
}

$artifactDir = Join-Path $root 'artifacts\fuzz'
New-Item -ItemType Directory -Force -Path $artifactDir | Out-Null
$savePath = Join-Path $artifactDir 'buus_fury_walk.sav'
$pngPath = Join-Path $artifactDir 'buus_fury_walk.png'

$oldStrict = $env:GBARECOMP_STRICT_STATIC
$oldDemo = $env:GBARECOMP_DEMO_INPUT
$oldView = $env:GBARECOMP_VIEW_WIDTH
try {
    $env:GBARECOMP_STRICT_STATIC = '1'
    $env:GBARECOMP_DEMO_INPUT = 'walk'
    $env:GBARECOMP_VIEW_WIDTH = $ViewWidth.ToString()
    & $exe --config (Join-Path $root 'game.toml') `
        --load-state $statePath --frames $Frames --save $savePath `
        --dump-png $pngPath
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    $env:GBARECOMP_STRICT_STATIC = $oldStrict
    $env:GBARECOMP_DEMO_INPUT = $oldDemo
    $env:GBARECOMP_VIEW_WIDTH = $oldView
}
