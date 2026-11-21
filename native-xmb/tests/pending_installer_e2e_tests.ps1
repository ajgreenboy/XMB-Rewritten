# pending_installer_e2e_tests.ps1
# ---------------------------------------------------------------------------
# Reproducible native end-to-end regression for the pending-installer lifecycle,
# driven through the real compiled app and the packaged helper (no mocked pages):
#   confirmation -> QProcess -> repack-install.ps1 -> synthetic installer ->
#   helper "exited" -> Verify/chooser -> registered Game row.
#
# Two modes (both use only benign synthetic artifacts; nothing real is executed):
#   -Mode single  (default): the installer writes ONE game executable, so Verify
#                 auto-registers it (single-exe path).
#   -Mode chooser : the installer writes TWO game executables, so Verify opens the
#                 chooser and the script selects one (multi-exe path).
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File pending_installer_e2e_tests.ps1 [-Mode single|chooser]
#     [-Exe <re-ps3-native.exe>] [-Helper <repack-install.ps1>] [-DevFlash <dev_flash>]
# ---------------------------------------------------------------------------
param(
    [ValidateSet('single','chooser')] [string]$Mode = 'single',
    [string]$Exe = 'C:\Users\Albert\.local\share\opencode\worktree\5d5811\pending-recheck-build\re-ps3-native.exe',
    [string]$Helper = 'C:\Users\Albert\.local\share\opencode\worktree\5d5811\pending-recheck-build\repack-install.ps1',
    [string]$DevFlash = 'C:\dev\rpcs3\bin\dev_flash'
)
$ErrorActionPreference = 'Stop'

$root = Join-Path $env:TEMP ('xmb-e2e-' + [guid]::NewGuid())
$library = Join-Path $root 'drive'
$games = Join-Path $library 'XMB Games'
$pending = Join-Path $games 'Pending Installers\e2e-synthetic'
$archiveDir = Join-Path $games 'Download Archives\e2e-synthetic'
$dataDir = Join-Path $root 'data'
New-Item -ItemType Directory -Force $pending, $archiveDir, $dataDir | Out-Null

try {
    # Build a benign synthetic WinForms installer whose Button1 writes the game
    # executables to /DIR and then closes (so the helper's UIA click completes it).
    if ($Mode -eq 'single') { $exeNames = 'SyntheticGame.exe' }
    else { $exeNames = 'SyntheticGame.exe','SyntheticLauncher.exe' }
    $writes = ($exeNames | ForEach-Object { 'File.WriteAllText(Path.Combine(dir,"' + $_ + '"),"synthetic");' }) -join ' '
    $source = @"
using System; using System.IO; using System.Windows.Forms;
public static class SyntheticInstaller {
  public static void Main(string[] args) {
    var f=new Form(); f.Name="SyntheticInstallerWindow"; f.Text="Synthetic Installer"; f.Width=480; f.Height=220;
    var b=new Button(); b.Name="Button1"; b.Text="Button1"; b.Left=150; b.Top=70; b.Width=180; f.Controls.Add(b);
    b.Click+=(s,e)=>{ string dir=Path.GetTempPath(); foreach(string a in args)if(a.StartsWith("/DIR=",StringComparison.OrdinalIgnoreCase))dir=a.Substring(5).Trim('"'); Directory.CreateDirectory(dir); $writes var t=new Timer(){Interval=800}; t.Tick+=(x,y)=>{t.Stop();f.Close();}; t.Start(); };
    Application.Run(f);
  }
}
"@
    $setup = Join-Path $pending 'setup.exe'
    Add-Type -TypeDefinition $source -OutputAssembly $setup -OutputType WindowsApplication -ReferencedAssemblies System.Windows.Forms,System.Drawing
    $hash = (Get-FileHash $setup -Algorithm SHA256).Hash.ToLowerInvariant()

    $registry = @{ version = 1; entries = @(@{ id='e2e-synthetic'; title='Synthetic E2E Game'; installer=$setup; archive=(Join-Path $archiveDir 'archive.zip'); destination=''; libraryRoot=$games; sha256=$hash; status='pending'; error='' }) } | ConvertTo-Json -Depth 5
    [IO.File]::WriteAllText((Join-Path $dataDir 'pending-installers.json'), $registry, (New-Object Text.UTF8Encoding($false)))

    # The packaged helper must sit beside the executable.
    $destHelper = Join-Path (Split-Path $Exe) 'repack-install.ps1'
    if ((Resolve-Path $Helper).Path -ne (Resolve-Path $destHelper).Path) {
        Copy-Item $Helper $destHelper -Force
    }

    $env:XMB_GAME_LIBRARY_ROOT = $library
    $env:XMB_LOG = Join-Path $root 'app.log'

    # Navigate: Game column -> pending row -> detail -> Install in -> confirm Yes -> launch helper.
    $nav = 'full,pause=4000,' + ('DOWN,pause=200,' * 18) + 'ENTER,pause=2000,ENTER,pause=2000,' + ('DOWN,pause=300,' * 3) + 'ENTER,pause=2000,RIGHT,pause=1000,ENTER,pause=12000'
    if ($Mode -eq 'chooser') {
        # Helper exited; Verify finds two candidates. From the detail page go up to
        # Verify Installation, open the chooser, pick the second candidate.
        $nav += ',UP,pause=500,ENTER,pause=1500,DOWN,pause=500,ENTER,pause=3000'
    }
    $tokens = $nav + ',quit'

    $p = Start-Process $Exe -ArgumentList @('--dev-flash', $DevFlash, '--data-dir', $dataDir, '--script', $tokens) -PassThru
    $p.WaitForExit()

    $final = Get-Content (Join-Path $dataDir 'pending-installers.json') -Raw | ConvertFrom-Json
    $entry = $final.entries | Where-Object id -eq 'e2e-synthetic'
    if ($null -eq $entry -or $entry.status -ne 'registered') {
        throw "pending installer did not reach 'registered' (status='$($entry.status)' error='$($entry.error)')"
    }

    $appsPath = Join-Path $dataDir 'apps.json'
    if (-not (Test-Path $appsPath)) { throw 'apps.json was not produced' }
    $apps = (Get-Content $appsPath -Raw | ConvertFrom-Json).apps
    $registered = @($apps | Where-Object id -eq 'release-game-e2e-synthetic')
    if ($registered.Count -ne 1) { throw "expected exactly one registered game row (found $($registered.Count))" }

    Write-Host "pending installer e2e [$Mode] passed: registered single row, executable=$($registered[0].executable)"
}
finally {
    Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -and $_.CommandLine -like "*$setup*" } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue
}
