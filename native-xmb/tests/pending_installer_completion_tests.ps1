# pending_installer_completion_tests.ps1
# ---------------------------------------------------------------------------
# Reproducible automatic-completion regression. Drives the real release-job
# pipeline (download -> extract -> stage -> register -> refresh) through the
# compiled app using a harmless loopback archive. It begins with no Pending
# Installers row, lets job completion stage/register a synthetic installer, and
# asserts the row appears in the SAME process without any manual refresh, and
# that the packaged helper is never launched (the job only stages, it does not
# install).
#
# The app must be built with the test hooks enabled (the env-gated loopback job
# injection is compiled out of release binaries):
#   cmake -DXMB_TEST_HOOKS=ON ...
#
# Caveats:
#   * The hook only accepts a literal loopback HTTP endpoint (127.0.0.1/localhost/
#     ::1, http only); a non-loopback URL is refused. The fixture serves the archive
#     directly (no redirect), so the download never leaves loopback.
#   * Recovery-manifest discovery is limited to configured library roots (the
#     XMB_GAME_LIBRARY_ROOT override plus queued job drives), never all drives; an
#     orphaned manifest on an unconfigured drive is not surfaced.
#   * These native scripts require the compiled app and a dev_flash firmware tree,
#     which the CI test runner does not provide, so they are dev-machine regressions
#     (CI already runs the helper lifecycle check and the C++ runner).
#
# Modes:
#   -Mode positive  (default): loopback URL is accepted and the pipeline runs.
#   -Mode negative  : a non-loopback URL is refused; no job is enqueued and no
#                     pending-installer record or row is produced.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File pending_installer_completion_tests.ps1 `
#     -Exe <re-ps3-native.exe> -DevFlash <dev_flash> [-Mode positive|negative]
# ---------------------------------------------------------------------------
param(
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string]$DevFlash,
    [ValidateSet('positive','negative')] [string]$Mode = 'positive',
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
    $jobUrl = ''
    if ($Mode -eq 'positive') {
        # 1. A synthetic ZIP archive containing a single setup.exe (never executed;
        #    the pipeline only detects and stages it).
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $zipExtract = Join-Path $root 'zipextract'
        New-Item -ItemType Directory -Force $zipExtract | Out-Null
        [IO.File]::WriteAllText((Join-Path $zipExtract 'setup.exe'), 'fixture')
        $zipPath = Join-Path $root 'archive.zip'
        [System.IO.Compression.ZipFile]::CreateFromDirectory($zipExtract, $zipPath)

        # 2. A loopback HTTP server serving the archive (127.0.0.1 only, no network,
        #    and no redirect).
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
        $jobUrl = "http://127.0.0.1:$port/archive.zip"
    } else {
        # Negative: a non-loopback host must be refused by the hook.
        $jobUrl = 'http://example.com/archive.zip'
    }

    $dump = Join-Path $root 'dump.json'
    $env:XMB_GAME_LIBRARY_ROOT = $library
    $env:XMB_PENDING_LOOPBACK_JOB_URL = $jobUrl
    $env:XMB_PENDING_LOOPBACK_JOB_TITLE = 'Completion Game'
    $env:XMB_DUMP_ENTRIES = $dump
    $env:XMB_LOG = Join-Path $root 'app.log'
    $tokens = 'full,pause=25000,quit'
    $p = Start-Process $Exe -ArgumentList @('--dev-flash', $DevFlash, '--data-dir', $dataDir, '--script', $tokens) -PassThru
    $p.WaitForExit()

    $registryPath = Join-Path $dataDir 'pending-installers.json'
    if ($Mode -eq 'negative') {
        # The hook must refuse the non-loopback URL: no record, no row, and the
        # refusal is logged (proving the hook ran and rejected it).
        if (Test-Path $registryPath) { throw 'non-loopback URL must not create a pending-installer record' }
        $log = Get-Content (Join-Path $root 'app.log') -Raw -ErrorAction SilentlyContinue
        if ($log -notmatch 'refusing non-loopback') { throw 'non-loopback URL was not explicitly refused by the hook' }
        if (Test-Path $dump) {
            $game = @(((Get-Content $dump -Raw | ConvertFrom-Json).categories | Where-Object id -eq 'game').entries)
            if (@($game | Where-Object id -eq 'game:pending-installers').Count -ne 0) {
                throw 'non-loopback URL must not surface a Pending Installers row'
            }
        }
        Write-Host 'pending installer completion negative test passed (non-loopback URL refused)'
        return
    }

    # 4. Assert the row appeared without a manual refresh.
    if (-not (Test-Path $dump)) { throw 'catalogue dump was not produced' }
    $game = @(((Get-Content $dump -Raw | ConvertFrom-Json).categories | Where-Object id -eq 'game').entries)
    $row = @($game | Where-Object id -eq 'game:pending-installers')
    if ($row.Count -ne 1) { throw "expected exactly one Pending Installers row (found $($row.Count))" }

    # 5. Assert the helper never launched: the record is staged as "pending",
    #    not "launching"/"registered", and its destination is still unset.
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
