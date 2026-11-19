param(
  [string]$Exe,
  [string]$FlashRoot = "C:\dev\rpcs3\bin\dev_flash",
  [string]$OutDir = "C:\dev\out\legacy-store-visual-polish"
)
# Reproduces the legacy-store visual-polish fixtures and runs the analytical geometry GATE.
# The captures are written OUTSIDE the repository (never committed); the geometry script and its
# report land next to them. This script is a gate: it fails on a nonzero native exit, on a missing
# capture, and on any geometry assertion failing (the geometry script exits nonzero). Usage:
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

# The native writes its logs to stderr, which PowerShell 5.1 otherwise surfaces as a NativeCommandError
# under -ErrorActionPreference Stop. Redirect both streams to a log and drop Stop for the run, then
# gate on the real exit code.
$log = Join-Path $OutDir 'capture.log'
$prev = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& $Exe --dev-flash "$FlashRoot" --data-dir "$OutDir\data" --script $script *> $log
$nativeExit = $LASTEXITCODE
$ErrorActionPreference = $prev
if ($nativeExit -ne 0) { throw "native capture failed with exit code $nativeExit (log: $log)" }

# Every expected capture must exist before the geometry gate runs, so a failed `grab=` is not
# silently accepted.
$expected = @('01-store-selected.png', '02-store-grid.png', '03-detail.png', '04-search-focus.png')
foreach ($f in $expected) {
  $p = Join-Path $OutDir $f
  if (-not (Test-Path -LiteralPath $p)) { throw "missing capture: $p" }
  if (-not (Test-Path -LiteralPath ($p + '.json'))) { throw "missing capture sidecar: $($p + '.json')" }
}

$bun = Join-Path $env:USERPROFILE '.bun\bin\bun.exe'
if (-not (Test-Path -LiteralPath $bun)) { $bun = 'bun' }
& $bun (Join-Path $PSScriptRoot 'legacy-store-geometry.ts') $OutDir
if ($LASTEXITCODE -ne 0) { throw "legacy-store-geometry.ts failed with exit code $LASTEXITCODE" }
