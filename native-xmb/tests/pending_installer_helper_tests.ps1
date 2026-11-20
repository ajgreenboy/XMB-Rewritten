$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$helper = Join-Path $repo 'packaging\repack-install.ps1'
$text = Get-Content -Raw $helper
foreach($required in @('Start-Process -FilePath $setupPath','-ArgumentList $innoArgs','FromHandle($proc.MainWindowHandle)','GetForegroundWindow','process-still-running','NORESTART','System.Windows.Forms','ProcessTree')) {
    if($text -notmatch [regex]::Escape($required)) { throw "helper contract missing: $required" }
}
if($text -match 'RootElement') { throw 'helper contains desktop-wide UI Automation root' }

function Invoke-Helper([string]$setup) {
    $scratch=Join-Path $env:TEMP ('xmb-pending-helper-'+[guid]::NewGuid())
    New-Item -ItemType Directory $scratch | Out-Null
    try {
        $out=Join-Path $scratch 'out.txt'; $err=Join-Path $scratch 'err.txt'
        $p=Start-Process powershell.exe -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',$helper,'-Setup',$setup,'-InstallDir',(Join-Path $scratch 'dest'),'-PageTimeoutMs','500','-InstallTimeoutMs','500') -Wait -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
        [pscustomobject]@{ Exit=$p.ExitCode; Out=(Get-Content $out -Raw -ErrorAction SilentlyContinue); Err=(Get-Content $err -Raw -ErrorAction SilentlyContinue) }
    } finally { Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue }
}

# Harmless executable probes: they never represent a downloaded installer.
$failed=Invoke-Helper (Join-Path $env:windir 'System32\where.exe')
if($failed.Exit -ne 3 -or $failed.Out -notmatch '"status":"failed"') { throw "no-window contract failed: exit=$($failed.Exit) output=$($failed.Out)" }
$success=Invoke-Helper (Join-Path $env:windir 'System32\rundll32.exe')
if($success.Exit -ne 0 -or $success.Out -notmatch '"status":"exited"') { throw "success contract failed: exit=$($success.Exit) output=$($success.Out)" }
Write-Host 'pending installer helper lifecycle contracts passed (synthetic executables only)'
