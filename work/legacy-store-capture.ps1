param(
  [string]$Exe,
  [string]$FlashRoot = "C:\dev\rpcs3\bin\dev_flash",
  [string]$OutDir = "C:\dev\out\native-hydra-xmb-e2e"
)
# Compatibility entry point for the former Store capture job. It now captures ordinary native
# Download Games pages and runs state/content checks; no legacy fixture, renderer, or visual baseline.
$ErrorActionPreference = 'Stop'
if (-not $Exe) { throw 'Pass -Exe pointing to the built re-ps3-native.exe' }
if (-not (Test-Path -LiteralPath $Exe)) { throw "Not found: $Exe" }
if (-not (Test-Path (Join-Path $FlashRoot 'vsh\resource\explore_plugin_full.rco'))) { throw 'Pass -FlashRoot pointing to your local PS3 dev_flash directory.' }
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

# This run is intentionally network-independent at the hub stage. A production loopback fixture
# can be supplied by the caller through XMB_HYDRA_TEST_FIXTURE and endpoint variables.
$env:XMB_INPUT_SCRIPT_ONLY = '1'
$downs = (1..18 | ForEach-Object { 'DOWN' }) -join ','
$script = "pause=3000,$downs,ENTER,pause=1200,grab=$OutDir\01-hub.png,quit"
$log = Join-Path $OutDir 'capture.log'
$prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
& $Exe --dev-flash "$FlashRoot" --data-dir "$OutDir\data" --script $script *> $log
$nativeExit = $LASTEXITCODE; $ErrorActionPreference = $prev
if ($nativeExit -ne 0) { throw "native capture failed with exit code $nativeExit (log: $log)" }

$bun = Join-Path $env:USERPROFILE '.bun\bin\bun.exe'; if (-not (Test-Path -LiteralPath $bun)) { $bun = 'bun' }
& $bun (Join-Path $PSScriptRoot 'legacy-store-geometry.ts') $OutDir
if ($LASTEXITCODE -ne 0) { throw "native Download Games capture gate failed" }
