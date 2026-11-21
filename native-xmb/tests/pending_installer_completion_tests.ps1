# pending_installer_completion_tests.ps1
# ---------------------------------------------------------------------------
# Reproducible automatic-completion regression: drives the real release-job
# pipeline (download -> extract -> stage -> register -> refresh) through the
# compiled app using a harmless loopback archive. It begins with no Pending
# Installers row, lets job completion stage/register a synthetic installer, and
# asserts the row appears in the SAME process without any manual refresh, and
# that the packaged helper is never launched (the job only stages, it does not
# install).
#
# Requires the app and firmware paths explicitly (no developer-absolute defaults).
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File pending_installer_completion_tests.ps1 `
#     -Exe <re-ps3-native.exe> -DevFlash <dev_flash> [-Helper <repack-install.ps1>]
# ---------------------------------------------------------------------------
param(
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string]$DevFlash,
    [string]$Helper = ''
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Exe)) { throw "Exe not found: $Exe" }
if (-not (Test-Path $DevFlash)) { throw "DevFlash not found: $DevFlash" }

$root = Join-Path $env:TEMP ('xmb-completion-' + [guid]::NewGuid())
$library = Join-Path $root 'drive'
$games = Join-Path $library 'XMB Games'
$dataDir = Join-Path $root 'data'
New-Item -ItemType Directory -Force $games, $dataDir | Out-Null

$serverJob = $null
try {
    # 1. A synthetic ZIP archive containing a single setup.exe (never executed;
    #    the pipeline only detects and stages it).
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zipExtract = Join-Path $root 'zipextract'
    New-Item -ItemType Directory -Force $zipExtract | Out-Null
    [IO.File]::WriteAllText((Join-Path $zipExtract 'setup.exe'), 'fixture')
    $zipPath = Join-Path $root 'archive.zip'
    [System.IO.Compression.ZipFile]::CreateFromDirectory($zipExtract, $zipPath)

    # 2. A loopback HTTP server serving the archive (127.0.0.1 only, no network).
    $port = Get-Random -Minimum 30000 -Maximum 50000
    $serverJob = Start-Job -ScriptBlock {
        param($port, $zipPath)
        $l = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $port)
        $l.Start()
        $bytes = [IO.File]::ReadAllBytes($zipPath)
        for ($i = 0; $i -lt 600; $i++) {
            if ($l.Pending()) {
                $c = $l.AcceptTcpClient(); $st = $c.GetStream()
                $b = [byte[]]::new(8192); $null = $st.Read($b, 0, 8192)
                $h = "HTTP/1.1 200 OK`r`nContent-Type: application/zip`r`nContent-Length: $($bytes.Length)`r`nConnection: close`r`n`r`n"
                $hb = [Text.Encoding]::ASCII.GetBytes($h)
                $st.Write($hb, 0, $hb.Length); $st.Write($bytes, 0, $bytes.Length); $st.Flush()
                $st.Close(); $c.Close()
            } else { Start-Sleep -Milliseconds 100 }
        }
        $l.Stop()
    } -ArgumentList $port, $zipPath
    Start-Sleep -Seconds 2

    # 3. Launch the app; the env-gated hook enqueues a job against the loopback
    #    archive, and the pipeline runs to completion. No REFRESH token is used.
    $dump = Join-Path $root 'dump.json'
    $env:XMB_GAME_LIBRARY_ROOT = $library
    $env:XMB_PENDING_LOOPBACK_JOB_URL = "http://127.0.0.1:$port/archive.zip"
    $env:XMB_PENDING_LOOPBACK_JOB_TITLE = 'Completion Game'
    $env:XMB_DUMP_ENTRIES = $dump
    $env:XMB_LOG = Join-Path $root 'app.log'
    $tokens = 'full,pause=25000,quit'
    $p = Start-Process $Exe -ArgumentList @('--dev-flash', $DevFlash, '--data-dir', $dataDir, '--script', $tokens) -PassThru
    $p.WaitForExit()

    # 4. Assert the row appeared without a manual refresh.
    if (-not (Test-Path $dump)) { throw 'catalogue dump was not produced' }
    $game = @(((Get-Content $dump -Raw | ConvertFrom-Json).categories | Where-Object id -eq 'game').entries)
    $row = @($game | Where-Object id -eq 'game:pending-installers')
    if ($row.Count -ne 1) { throw "expected exactly one Pending Installers row (found $($row.Count))" }

    # 5. Assert the helper never launched: the record is staged as "pending",
    #    not "launching"/"registered", and its destination is still unset.
    $registryPath = Join-Path $dataDir 'pending-installers.json'
    if (-not (Test-Path $registryPath)) { throw 'pending-installers.json was not produced' }
    $entries = @((Get-Content $registryPath -Raw | ConvertFrom-Json).entries)
    $entry = @($entries | Where-Object title -eq 'Completion Game')[0]
    if ($null -eq $entry) { throw "the completion job did not produce a pending-installer record (entries=$($entries.Count))" }
    if ($entry.status -ne 'pending') { throw "record status must be 'pending' (staged, not installed), got '$($entry.status)'" }
    if ($entry.destination) { throw "record destination must remain empty (helper never launched), got '$($entry.destination)'" }

    Write-Host "pending installer automatic-completion regression passed (row appeared without manual refresh; helper never launched)"
}
finally {
    if ($serverJob) { Stop-Job $serverJob -ErrorAction SilentlyContinue; Remove-Job $serverJob -Force -ErrorAction SilentlyContinue }
    Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue
}
