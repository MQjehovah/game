<#
Showcase render-regression suite: captures deterministic nature scenes and
diffs them against committed golden PNGs.

Only scenes without random particle scripts belong here (the snowfall, dust,
fireflies and aurora scripts use math.random, so their pixels legitimately
change between runs). 04_lakeside and 06_ocean are static geometry + animated
water/sky, which converge to a stable frame.

Usage:
  powershell -NoProfile -File tools/render_regression_showcase.ps1
  powershell -NoProfile -File tools/render_regression_showcase.ps1 -UpdateGolden

The default build is the MSVC one used by this repo. Pass -Build to override.
#>
param(
    [string]$Build = "build-msvc/Release",
    [int]$Frame = 300,
    [switch]$UpdateGolden
)

$ErrorActionPreference = "Stop"
$runner = Join-Path $PSScriptRoot "render_regression.ps1"

$cases = @(
    @{ Name = "04_lakeside"; Project = "projects/showcase";
       Scene = "assets/scenes/04_lakeside.json";
       Golden = "tests/golden/showcase_04_lakeside_300.png" },
    @{ Name = "06_ocean"; Project = "projects/showcase";
       Scene = "assets/scenes/06_ocean.json";
       Golden = "tests/golden/showcase_06_ocean_300.png" }
)

$failed = $false
foreach ($case in $cases) {
    Write-Host ""
    Write-Host "=== $($case.Name) ==="
    & $runner -Project $case.Project -Scene $case.Scene -Golden $case.Golden `
        -Frame $Frame -Build $Build -Tol 8 -MaxRatio 0.001 -UpdateGolden:$UpdateGolden
    if ($LASTEXITCODE -ne 0) { $failed = $true }
}

if ($failed) {
    Write-Host "showcase render regression: FAIL"
    exit 1
}
Write-Host "showcase render regression: PASS"
