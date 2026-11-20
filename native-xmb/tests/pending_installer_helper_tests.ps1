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
        $p=Start-Process powershell.exe -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',$helper,'-Setup',('"{0}"' -f $setup),'-InstallDir',('"{0}"' -f $install),'-PageTimeoutMs','5000','-InstallTimeoutMs',$timeout) -Wait -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
        [pscustomobject]@{ Exit=$p.ExitCode; Out=(Get-Content $out -Raw -ErrorAction SilentlyContinue); Err=(Get-Content $err -Raw -ErrorAction SilentlyContinue) }
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
    string dir=Path.Combine(Path.GetTempPath(),"xmb-synthetic-install"); bool child=false, exitRoot=false;
    foreach(string arg in args) { if(arg=="--child") child=true; if(arg=="--exit-root") exitRoot=true; if(arg.StartsWith("/DIR=",StringComparison.OrdinalIgnoreCase)) dir=arg.Substring(5).Trim('"'); }
    if(dir.IndexOf("timeout",StringComparison.OrdinalIgnoreCase)>=0){child=true;exitRoot=true;}
    var form=new Form(); form.Name="SyntheticInstallerWindow"; form.Text="Synthetic Installer"; form.Width=420; form.Height=180;
    var button=new Button(); button.Name="Button1"; button.Text="Button1"; button.Left=130; button.Top=50; button.Width=140; form.Controls.Add(button);
    button.Click += (s,e) => { Directory.CreateDirectory(dir); File.WriteAllText(Path.Combine(dir,"SyntheticGame.exe"),"fixture"); if(child) Process.Start(new ProcessStartInfo("powershell.exe","-NoProfile -Command Start-Sleep -Seconds 20"){UseShellExecute=false}); if(exitRoot) form.Close(); else { var timer=new Timer(){Interval=500,Tag=form}; timer.Tick += (x,y)=>{ timer.Stop(); form.Close(); }; timer.Start(); } };
    Application.Run(form);
  }
}
'@
    $exe=Join-Path $fixture 'SyntheticInstaller.exe'
    Add-Type -TypeDefinition $source -OutputAssembly $exe -OutputType WindowsApplication -ReferencedAssemblies System.Windows.Forms,System.Drawing
    function Read-Protocol($text) { $lines=@($text -split "`r?`n" | Where-Object {$_.Trim()}); for($i=$lines.Count-1;$i -ge 0;$i--){ try { $v=$lines[$i] | ConvertFrom-Json; if($v.status){return $v} } catch {} }; return $null }
    # The fixture is a real WinForms/UIA process. The normal case validates
    # AutomationId Button1, quoted paths with spaces, and structured exited.
    $normal=Invoke-Helper $exe
    $normalJson=Read-Protocol $normal.Out
    if($normal.Exit -ne 0 -or $normalJson.status -ne 'exited') { throw "GUI exited contract failed: exit=$($normal.Exit) output=$($normal.Out)" }
    if($normalJson.processIds.Count -lt 0) { throw 'GUI protocol processIds missing' }
    Write-Host 'synthetic GUI installer exited protocol passed'
    $timeout=Invoke-Helper $exe 'timeout fixture with spaces' 1000
    $timeoutJson=Read-Protocol $timeout.Out
    if($timeout.Exit -ne 4 -or $timeoutJson.status -notin @('timeout','process-still-running') -or @($timeoutJson.processIds).Count -lt 1) { throw "GUI child timeout contract failed: exit=$($timeout.Exit) output=$($timeout.Out) error=$($timeout.Err)" }
    Write-Host 'synthetic GUI child timeout protocol passed'
} finally { Remove-Item $fixture -Recurse -Force -ErrorAction SilentlyContinue }
Write-Host 'pending installer helper lifecycle contracts passed (synthetic GUI fixture)'
