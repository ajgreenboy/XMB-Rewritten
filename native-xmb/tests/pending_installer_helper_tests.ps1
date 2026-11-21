$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$helper = Join-Path $repo 'packaging\repack-install.ps1'
$text = Get-Content -Raw $helper
foreach($required in @('Start-Process -FilePath $setupPath','-ArgumentList $innoArgs','FromHandle($window)','GetForegroundWindow','process-still-running','NORESTART','System.Windows.Forms','ProcessTree','processIdentities')) {
    if($text -notmatch [regex]::Escape($required)) { throw "helper contract missing: $required" }
}
if($text -match 'RootElement') { throw 'helper contains desktop-wide UI Automation root' }

function Invoke-Helper([string]$setup,[string]$label='normal',[int]$timeout=10000) {
    $scratch=Join-Path $env:TEMP ('xmb-pending-helper-'+[guid]::NewGuid())
    New-Item -ItemType Directory $scratch | Out-Null
    try {
        $out=Join-Path $scratch 'out.txt'; $err=Join-Path $scratch 'err.txt'
        $install=Join-Path $scratch $label
        $p=Start-Process powershell.exe -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',$helper,'-Setup',('"{0}"' -f $setup),'-InstallDir',('"{0}"' -f $install),'-PageTimeoutMs','5000','-InstallTimeoutMs',$timeout) -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
        $waitUntil=[DateTime]::UtcNow.AddSeconds(30)
        while(-not $p.HasExited -and [DateTime]::UtcNow -lt $waitUntil) { Start-Sleep -Milliseconds 100; $p.Refresh() }
        if(-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; throw "$label helper exceeded its own 30 second test bound" }
        $p.WaitForExit()
        $rawOut=Get-Content $out -Raw -ErrorAction SilentlyContinue
        $protocol=Read-Protocol $rawOut
        [pscustomobject]@{ Exit=$(if($protocol){[int]$protocol.exitCode}else{$p.ExitCode}); Out=$rawOut; Err=(Get-Content $err -Raw -ErrorAction SilentlyContinue) }
    } finally { Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue }
}

$fixture=Join-Path $env:TEMP ('xmb-synthetic-gui-installer-'+[guid]::NewGuid())
New-Item -ItemType Directory $fixture | Out-Null
try {
    $source=@'
using System;
using System.Diagnostics;
using System.IO;
using System.Windows.Forms;
public static class SyntheticInstaller {
  public static void Main(string[] args) {
    string executableName=Path.GetFileNameWithoutExtension(Application.ExecutablePath);
    if(executableName.IndexOf("NoWindow",StringComparison.OrdinalIgnoreCase)>=0) return;
    if(executableName.IndexOf("Nonzero",StringComparison.OrdinalIgnoreCase)>=0) Environment.Exit(7);
    if(Array.Exists(args,a=>a=="--unrelated-window")) { string log=Path.Combine(Path.GetTempPath(),"xmb-unrelated-click.log"); foreach(string arg in args)if(arg.StartsWith("--unrelated-log="))log=arg.Substring(17); var unrelated=new Form(); unrelated.Text="Unrelated Window"; unrelated.Name="UnrelatedWindow"; unrelated.Width=360; unrelated.Height=140; var unrelatedButton=new Button(); unrelatedButton.Name="Button1"; unrelatedButton.Text="UnrelatedButton"; unrelated.Controls.Add(unrelatedButton); unrelatedButton.Click+=(s,e)=>File.WriteAllText(log,"clicked"); Application.Run(unrelated); return; }
    string dir=Path.Combine(Path.GetTempPath(),"xmb-synthetic-install"); bool child=false, exitRoot=false;
    foreach(string arg in args) { if(arg=="--child") child=true; if(arg=="--exit-root") exitRoot=true; if(arg.StartsWith("/DIR=",StringComparison.OrdinalIgnoreCase)) dir=arg.Substring(5).Trim('"'); }
    if(dir.IndexOf("timeout",StringComparison.OrdinalIgnoreCase)>=0){child=true;exitRoot=true;}
    if(Array.Exists(args,a=>a=="--child-window")) { var childForm=new Form(); childForm.Text="Synthetic Child Installer"; childForm.Name="SyntheticChildWindow"; childForm.Width=360; childForm.Height=140; var childButton=new Button(); childButton.Name="ChildButton"; childButton.Text="ChildButton"; childForm.Controls.Add(childButton); Application.Run(childForm); return; }
    var form=new Form(); form.Name="SyntheticInstallerWindow"; form.Text="Synthetic Installer"; form.Width=420; form.Height=180;
    var button=new Button(); button.Name="Button1"; button.Text="Button1"; button.Left=130; button.Top=50; button.Width=140; form.Controls.Add(button);
    button.Click += (s,e) => { Directory.CreateDirectory(dir); File.WriteAllText(Path.Combine(dir,"SyntheticGame.exe"),"fixture"); if(child) Process.Start(new ProcessStartInfo(Application.ExecutablePath,"--child-window"){UseShellExecute=false}); if(exitRoot) { if(child) { var timer=new Timer(){Interval=7000,Tag=form}; timer.Tick += (x,y)=>{ timer.Stop(); form.Close(); }; timer.Start(); } else { form.Close(); } } else { var timer=new Timer(){Interval=500,Tag=form}; timer.Tick += (x,y)=>{ timer.Stop(); form.Close(); }; timer.Start(); } };
    Application.Run(form);
  }
}
'@
    $exe=Join-Path $fixture 'SyntheticInstaller.exe'
    Add-Type -TypeDefinition $source -OutputAssembly $exe -OutputType WindowsApplication -ReferencedAssemblies System.Windows.Forms,System.Drawing
    Copy-Item $exe (Join-Path $fixture 'SyntheticNoWindow.exe')
    Copy-Item $exe (Join-Path $fixture 'SyntheticNonzero.exe')
    function Read-Protocol($text) { $lines=@($text -split "`r?`n" | Where-Object {$_.Trim()}); for($i=$lines.Count-1;$i -ge 0;$i--){ try { $v=$lines[$i] | ConvertFrom-Json; if($v.status){return $v} } catch {} }; return $null }
    # The fixture is a real WinForms/UIA process. The normal case validates
    # AutomationId Button1, quoted paths with spaces, and structured exited.
    $unrelatedLog=Join-Path $fixture 'unrelated-click.log'
    $unrelated=Start-Process (Join-Path $fixture 'SyntheticInstaller.exe') -ArgumentList @('--unrelated-window',"--unrelated-log=$unrelatedLog") -PassThru
    Start-Sleep -Milliseconds 500
    $normal=Invoke-Helper $exe
    $normalJson=Read-Protocol $normal.Out
    if($normal.Exit -ne 0 -or $normalJson.status -ne 'exited') { throw "GUI exited contract failed: exit=$($normal.Exit) output=$($normal.Out)" }
    if($normalJson.processIds.Count -lt 0) { throw 'GUI protocol processIds missing' }
    if(Test-Path $unrelatedLog) { throw 'helper interacted with an unrelated window' }
    Stop-Process -Id $unrelated.Id -Force -ErrorAction SilentlyContinue
    Write-Host 'synthetic GUI installer exited protocol passed'
    $noWindow=Invoke-Helper (Join-Path $fixture 'SyntheticNoWindow.exe') 'no-window fixture'
    $noWindowJson=Read-Protocol $noWindow.Out
    if($noWindow.Exit -ne 3 -or $noWindowJson.status -ne 'no-window') { throw "no-window protocol failed: exit=$($noWindow.Exit) output=$($noWindow.Out)" }
    $nonzero=Invoke-Helper (Join-Path $fixture 'SyntheticNonzero.exe') 'nonzero fixture'
    $nonzeroJson=Read-Protocol $nonzero.Out
    if($nonzero.Exit -ne 5 -or $nonzeroJson.status -ne 'nonzero') { throw "nonzero protocol failed: exit=$($nonzero.Exit) output=$($nonzero.Out)" }
    Write-Host 'synthetic no-window/nonzero protocols passed'
    $timeout=Invoke-Helper $exe 'timeout fixture with spaces' 5000
    $timeoutJson=Read-Protocol $timeout.Out
    if($timeout.Exit -ne 4 -or $timeoutJson.status -notin @('timeout','process-still-running') -or @($timeoutJson.processIds).Count -lt 1) { throw "GUI child timeout contract failed: exit=$($timeout.Exit) output=$($timeout.Out) error=$($timeout.Err)" }
    $timeoutIdentity=@($timeoutJson.processIdentities)[0]
    if($null -eq $timeoutIdentity -or $timeoutIdentity.creationTime -isnot [string]) { throw "processIdentity creationTime must be a lossless decimal string, not a number (FILETIME exceeds double precision)" }
    if([long]$timeoutIdentity.creationTime -le 9007199254740992) { throw "processIdentity creationTime did not exceed 2^53; FILETIME precision is not exercisable" }
    Write-Host 'synthetic GUI child timeout protocol passed (lossless FILETIME string)'
} finally {
    Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
        Where-Object {$_.CommandLine -and $_.CommandLine -like "*$fixture*"} |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    Remove-Item $fixture -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host 'pending installer helper lifecycle contracts passed (synthetic GUI fixture)'
