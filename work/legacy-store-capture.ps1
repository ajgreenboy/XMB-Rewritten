param(
  [string]$Exe,
  [string]$FlashRoot = "C:\dev\rpcs3\bin\dev_flash",
  [string]$OutDir = "C:\dev\out\legacy-store-visual-polish"
)
# Reproduces the legacy-store visual-polish fixtures and runs the analytical geometry check.
# The captures are written OUTSIDE the repository (never committed); the geometry script and its
# report land next to them. Usage:
#   pwsh -File work\legacy-store-capture.ps1 -Exe <worktree>\out\native-xmb\re-ps3-native.exe
$ErrorActionPreference = 'Stop'
if (-not $Exe) { throw 'Pass -Exe pointing to the built re-ps3-native.exe' }
if (-not (Test-Path -LiteralPath $Exe)) { throw "Not found: $Exe" }
if (-not (Test-Path (Join-Path $FlashRoot 'vsh\resource\explore_plugin_full.rco'))) { throw 'Pass -FlashRoot pointing to your local PS3 dev_flash directory.' }
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

$env:XMB_LEGACY_STORE_FIXTURE = '1'
# From a fresh boot the script walks the Game column to Browse Popular and opens the legacy store
# (the same route the legacy-store-ui-check manifest documents). XMB_LEGACY_STORE_FIXTURE=1
# replaces HydraLibrary::popularGames with 14 synthetic games (solid-colour covers), so no network.
$downs = (1..18 | ForEach-Object { 'DOWN' }) -join ','
$script = "pause=4000,$downs,UP,ENTER,DOWN,ENTER,pause=1500,grab=$OutDir\01-store-selected.png,RIGHT,grab=$OutDir\02-store-grid.png,ENTER,pause=600,grab=$OutDir\03-detail.png,ESC,pause=400,UP,grab=$OutDir\04-search-focus.png,quit"
& $Exe --dev-flash "$FlashRoot" --data-dir "$OutDir\data" --script $script

$bun = Join-Path $env:USERPROFILE '.bun\bin\bun.exe'
if (-not (Test-Path -LiteralPath $bun)) { $bun = 'bun' }
& $bun (Join-Path $PSScriptRoot 'legacy-store-geometry.ts') $OutDir
