# pending_installer_row_tests.ps1
# ---------------------------------------------------------------------------
# Proves the Game column's "Pending Installers" row is reconciled on a
# same-session content refresh, not only at construction: the row is absent when
# there are no pending artifacts, then appears (without a restart) once a
# recovery-manifest is created and the catalogue is refreshed, with no duplicate.
#
# Uses only a benign synthetic recovery manifest (no installer is executed).
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File pending_installer_row_tests.ps1
#     [-Exe <path-to-re-ps3-native.exe>] [-DevFlash <path-to-dev_flash>]
# ---------------------------------------------------------------------------
param(
    [string]$Exe = 'C:\Users\Albert\.local\share\opencode\worktree\5d5811\pending-recheck-build\re-ps3-native.exe',
    [string]$DevFlash = 'C:\dev\rpcs3\bin\dev_flash'
)
$ErrorActionPreference = 'Stop'

$root = Join-Path $env:TEMP ('xmb-row-reg-' + [guid]::NewGuid())
$library = Join-Path $root 'drive'
$games = Join-Path $library 'XMB Games'
$pendingDir = Join-Path $games 'Pending Installers\e2e-row'
$dataDir = Join-Path $root 'data'
New-Item -ItemType Directory -Force $pendingDir, $dataDir | Out-Null

try {
    # A benign synthetic recovery manifest. The row discovery only checks that the
    # journal exists inside an owned Pending Installers/<id>/ folder.
    $manifest = Join-Path $pendingDir '.xmb-pending-recovery.json'
    $manifestJson = @{
        id = 'e2e-row'; title = 'Row Game';
        installer = (Join-Path $pendingDir 'setup.exe');
        archive = (Join-Path $games 'Download Archives\e2e-row\archive.zip');
        libraryRoot = $games
    } | ConvertTo-Json -Compress

    $dump = Join-Path $root 'dump.json'
    $env:XMB_GAME_LIBRARY_ROOT = $library
    $env:XMB_DUMP_ENTRIES = $dump
    $env:XMB_LOG = Join-Path $root 'app.log'

    # full -> first content refresh (row absent); a long pause is the injection
    # window; REFRESH (F5) reruns refreshContent (row present).
    $tokens = 'full,pause=4000,pause=14000,REFRESH,pause=5000,quit'
    $p = Start-Process $Exe -ArgumentList @('--dev-flash', $DevFlash, '--data-dir', $dataDir, '--script', $tokens) -PassThru

    $before = Join-Path $root 'before.json'
    $deadline = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $deadline -and -not (Test-Path $dump)) { Start-Sleep -Milliseconds 200 }
    if (-not (Test-Path $dump) -or (Get-Item $dump).Length -eq 0) { Stop-Process $p -Force -ErrorAction SilentlyContinue; throw 'first catalogue dump never appeared' }
    Copy-Item $dump $before

    # Inject the recovery manifest while the same process is still alive.
    [IO.File]::WriteAllText($manifest, $manifestJson, (New-Object Text.UTF8Encoding($false)))

    $p.WaitForExit()

    function Get-GameEntries([string]$path) {
        $j = Get-Content $path -Raw | ConvertFrom-Json
        return @(($j.categories | Where-Object id -eq 'game').entries)
    }

    $beforeRows = @(Get-GameEntries $before | Where-Object id -eq 'game:pending-installers')
    $afterRows = @(Get-GameEntries $dump | Where-Object id -eq 'game:pending-installers')

    if ($beforeRows.Count -ne 0) { throw "pending row should be absent before artifacts exist (found $($beforeRows.Count))" }
    if ($afterRows.Count -ne 1) { throw "pending row should appear exactly once after a same-session refresh (found $($afterRows.Count))" }

    Write-Host 'pending installer row same-session refresh passed (before=absent, after=present, no duplicate)'
}
finally {
    Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue
}
