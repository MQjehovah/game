<#
Render regression: capture one frame from the standalone player and diff it
against a committed golden PNG with neon_pixel_diff. The script exits non-zero
when the differing-pixel ratio exceeds -MaxRatio, so CI (or a pre-commit hook)
can gate on rendered output instead of eyeballing screenshots.

Capture determinism: the player is fixed-step, so the same build + frame yields
a near-identical image (measured: ~0.003% pixels differ run to run, mostly HUD
antialiasing). Keep -Tol/-MaxRatio tight enough to catch a real regression and
loose enough to absorb that noise.

Add a case: capture a golden once with -UpdateGolden, review it, commit it.

Example:
  pwsh tools/render_regression.ps1 `
    -Project projects/physics_sandbox `
    -Scene assets/scenes/sandbox.json `
    -Golden tests/golden/sandbox_900.png -Frame 900
#>
param(
    [Parameter(Mandatory = $true)][string]$Project,   # e.g. projects/physics_sandbox
    [Parameter(Mandatory = $true)][string]$Scene,     # project-relative, e.g. assets/scenes/sandbox.json
    [Parameter(Mandatory = $true)][string]$Golden,    # committed reference png
    [int]$Frame = 900,
    [int]$Smoke = 0,                                  # 0 -> Frame + 60
    [int]$Tol = 8,
    [double]$MaxRatio = 0.001,
    [string]$Build = "build/mingw-ninja",
    [switch]$UpdateGolden
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$player = Join-Path $repoRoot (Join-Path $Build "neon_game.exe")
$differ = Join-Path $repoRoot (Join-Path $Build "neon_pixel_diff.exe")
if (-not (Test-Path $player)) { Write-Error "player not found: $player"; exit 2 }
if (-not (Test-Path $differ)) { Write-Error "pixel differ not found: $differ"; exit 2 }
if ($Smoke -le 0) { $Smoke = $Frame + 60 }

$goldenPath = if ([System.IO.Path]::IsPathRooted($Golden)) { $Golden } else { Join-Path $repoRoot $Golden }
$actual = Join-Path $repoRoot (Join-Path $Build "render_regression_actual.png")
$diffOut = Join-Path $repoRoot (Join-Path $Build "render_regression_diff.png")
Remove-Item -LiteralPath $actual -ErrorAction SilentlyContinue

Write-Host "capture: --scene $Project/$Scene --scripts $Project --frame $Frame"
& $player --scene "$Project/$Scene" --scripts $Project --smoke-test $Smoke --screenshot $actual $Frame | Out-Null
if (-not (Test-Path $actual)) { Write-Error "capture produced no image"; exit 2 }

if ($UpdateGolden -or -not (Test-Path $goldenPath)) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $goldenPath) | Out-Null
    Copy-Item -LiteralPath $actual -Destination $goldenPath -Force
    Write-Host "golden written: $goldenPath (review + commit it)"
    exit 0
}

& $differ $goldenPath $actual $Tol $MaxRatio $diffOut
$code = $LASTEXITCODE
if ($code -ne 0) { Write-Host "render regression: FAIL (diff -> $diffOut)" } else { Write-Host "render regression: PASS" }
exit $code
