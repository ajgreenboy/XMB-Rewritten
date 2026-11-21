<#
    repack-install.ps1
    ----------------------------------------------------------------------
    Drive a FitGirl or DODI repack "setup.exe" through its installer by
    remote-controlling the window (Windows UI Automation). There is no
    /SILENT switch for these repacks: they are Inno Setup wrapping a custom
    ISDone decompression screen that ignores Inno flags. The usual Inno
    flags are still passed (they suppress the ordinary wizard pages), and
    everything left over is clicked by this script.

    Usage
    ----------------------------------------------------------------------
        .\repack-install.ps1 -Setup <path-to-setup.exe> -InstallDir <dir> [options]

    Example
        .\repack-install.ps1 `
            -Setup "D:\Downloads\Game Repack\setup.exe" `
            -InstallDir "D:\Games\Game Name"

    DODI-specific
    ----------------------------------------------------------------------
    DODI installers are known to render their Install button off-screen at
    some resolutions. The arrow-key fallback (focus the window, press UP)
    is handled automatically when -Dodi is set.

    IMPORTANT / LIMITATIONS
    ----------------------------------------------------------------------
    * This is UI automation: the installer GUI actually runs and is clicked.
      It is not a true silent install. Keep the machine usable or lock it.
    * Decompression can run for a long time. -InstallTimeoutMs must cover it.
    * Installers must run elevated when they write to a protected path.
      Re-run this script from an Administrator PowerShell if you see a UAC
      prompt or a permission failure inside the installer.
    * Run with -WhatIf first to see what would happen without clicking.
#>

[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)] [string]$Setup,        # full/relative path to setup.exe
    [Parameter(Mandatory)] [string]$InstallDir,   # target directory
    [Alias('Dodi')]        [switch]$IsDodi,       # DODI quirks (up-arrow fallback)
    [int]    $InstallTimeoutMs = 7200000,         # 2h default; fits large repacks
    [int]    $PageTimeoutMs    = 600000,          # wait for a page before failing
    [string] $Lang             = 'en',
    [switch] $RevertUac                           # elevation through explorer is intentionally unsupported
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Windows.Forms
Add-Type @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
public static class ProcessTree {
  [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr CreateToolhelp32Snapshot(uint flags,uint pid);
  [DllImport("kernel32.dll")] static extern bool Process32First(IntPtr snap, ref Entry e);
  [DllImport("kernel32.dll")] static extern bool Process32Next(IntPtr snap, ref Entry e);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenProcess(uint access,bool inherit,uint pid);
  [DllImport("kernel32.dll")] static extern bool GetProcessTimes(IntPtr h,out System.Runtime.InteropServices.ComTypes.FILETIME created,out System.Runtime.InteropServices.ComTypes.FILETIME exited,out System.Runtime.InteropServices.ComTypes.FILETIME kernel,out System.Runtime.InteropServices.ComTypes.FILETIME user);
  [StructLayout(LayoutKind.Sequential)] public struct Node { public int Pid; public int ParentPid; public long CreationTime; }
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] struct Entry { public uint Size,Usage,Pid; public IntPtr DefaultHeap; public uint Module,Threads,ParentPid; public int Priority; public uint Flags; [MarshalAs(UnmanagedType.ByValTStr, SizeConst=260)] public string Exe; }
  public static long CreationTime(int pid) { IntPtr h=OpenProcess(0x1000,false,(uint)pid); if(h==IntPtr.Zero)return 0; System.Runtime.InteropServices.ComTypes.FILETIME c,x,k,u; bool ok=GetProcessTimes(h,out c,out x,out k,out u); CloseHandle(h); if(!ok)return 0; return ((long)c.dwHighDateTime<<32)+(uint)c.dwLowDateTime; }
  public static Node[] Snapshot() { var result=new List<Node>(); IntPtr s=CreateToolhelp32Snapshot(2,0); if(s==new IntPtr(-1)) return result.ToArray(); var e=new Entry(); e.Size=(uint)Marshal.SizeOf(e); if(Process32First(s,ref e)) do { result.Add(new Node{Pid=(int)e.Pid,ParentPid=(int)e.ParentPid,CreationTime=CreationTime((int)e.Pid)}); } while(Process32Next(s,ref e)); CloseHandle(s); return result.ToArray(); }
}
'@
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class UiTarget {
  [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr h,uint msg,IntPtr w,IntPtr l);
  public static void Click(int hwnd) { if(hwnd!=0) SendMessage(new IntPtr(hwnd),0x00F5,IntPtr.Zero,IntPtr.Zero); }
}
'@

function Write-Result {
    param([string]$Status,[string]$Message,[int]$ExitCode = 0,[int[]]$ProcessIds=@())
    $identities=@(); $uniqueIds=@($ProcessIds | Sort-Object -Unique)
    foreach($processId in @($uniqueIds)) {
        $key=[string]$processId
        if($script:verifiedIdentities -and $script:verifiedIdentities.ContainsKey($key)) {
            # FILETIME (100ns ticks since 1601) exceeds double's exact integer
            # range, so it must travel as a lossless decimal string, never a JSON
            # number that would drop the low bits on the native side.
            $identities+=@{pid=$processId;creationTime=[string]$script:verifiedIdentities[$key]}
        } else {
            try { $p=Get-Process -Id $processId -ErrorAction Stop; $identities+=@{pid=$processId;creationTime=[string]$p.StartTime.ToUniversalTime().ToFileTimeUtc()} } catch {}
        }
    }
    $complete=(@($identities | ForEach-Object {[int]$_.pid} | Sort-Object -Unique).Count -eq $uniqueIds.Count)
    if(-not $complete -and $uniqueIds.Count -gt 0) { $Status='manual-review'; $Message='A verified live process identity could not be captured; duplicate launch is blocked until manual review.'; $ExitCode=6 }
    [Console]::Out.WriteLine((@{ status=$Status; message=$Message; exitCode=$ExitCode; processIds=@($uniqueIds); processIdentities=@($identities) } | ConvertTo-Json -Compress))
    if ($ExitCode -ne 0) { exit $ExitCode }
}

# ---------------------------------------------------------------------------
# UI Automation helpers
# ---------------------------------------------------------------------------

# Find an element by one or more AutomationId candidates. Returns $null if
# none found within the timeout (0 = no wait, check immediately).
function Find-ElementByAutomationId {
    param(
        [string[]]$Ids,
        [System.Windows.Automation.AutomationElement]$Root,
        [int]$TimeoutMs = 0
    )
    if (-not $Root) { throw "Installer UI root is required; refusing desktop-wide automation." }

    # Build an OR of the candidate AutomationIds.
    $orCond = $null
    foreach ($id in $Ids) {
        $automationId = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $id)
        $name = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::NameProperty, $id)
        $c = New-Object System.Windows.Automation.OrCondition($automationId, $name)
        if (-not $orCond) { $orCond = $c }
        else { $orCond = New-Object System.Windows.Automation.OrCondition($orCond, $c) }
    }
    if (-not $orCond) { return $null }

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    do {
        try {
            $el = $Root.FindFirst(
                [System.Windows.Automation.TreeScope]::Descendants, $orCond)
            if ($el) { return $el }
        } catch { }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)
    return $null
}

function Get-CurrentPattern {
    param([System.Windows.Automation.AutomationElement]$el)
    try { return $el.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern) }
    catch { return $null }
}

function Invoke-Click {
    param([System.Windows.Automation.AutomationElement]$el)
    $p = Get-CurrentPattern $el
    if ($p) {
        $p.Invoke()
    } else {
        # Fallback: keyboard navigation is handled by the caller via Space/Enter.
        $hwnd=$el.Current.NativeWindowHandle
        if($hwnd -ne 0) { [UiTarget]::Click($hwnd) } else { throw "UIA control has no invokable pattern or native target." }
    }
}

# Wait until the setup process has a visible main window.
function Wait-SetupWindow {
    param([System.Diagnostics.Process]$Process, [int]$TimeoutMs)
    $deadline = [DateTime]::UtcNow.AddMilliseconds([Math]::Min($TimeoutMs,(Get-RemainingMs)))
    do {
        $handle=Get-VerifiedWindowHandle -RootId $Process.Id
        if ($handle -ne 0) { return $handle }
        Start-Sleep -Milliseconds 500
    } while ([DateTime]::UtcNow -lt $deadline)
    return [IntPtr]::Zero
}

function Get-DescendantProcessIds {
    param([int[]]$ParentIds)
    foreach($parent in @($ParentIds)) { if(-not $script:verifiedIdentities.ContainsKey([string]$parent)) { try {$created=(Get-Process -Id $parent -ErrorAction Stop).StartTime.ToUniversalTime().ToFileTimeUtc()} catch {$created=[ProcessTree]::CreationTime($parent)}; if($created -gt 0){$script:verifiedIdentities[[string]$parent]=$created} } }
    $changed=$true; $passes=0
    while($changed -and (Get-RemainingMs) -gt 0 -and $passes++ -lt 64) {
        $changed=$false
        $nodes=[ProcessTree]::Snapshot()
        foreach($node in $nodes) {
            $parentKey=[string]$node.ParentPid
            # Validate the current parent's creation identity, then capture the
            # child's own identity. A child's timestamp can never equal its
            # parent's timestamp; comparing those two was an identity bug.
            if(-not $script:verifiedIdentities.ContainsKey($parentKey)) { continue }
            $parentTime=0
            try {$parentTime=(Get-Process -Id $node.ParentPid -ErrorAction Stop).StartTime.ToUniversalTime().ToFileTimeUtc()} catch {continue}
            $nodeTime=0
            try {$nodeTime=(Get-Process -Id $node.Pid -ErrorAction Stop).StartTime.ToUniversalTime().ToFileTimeUtc()} catch {continue}
            if([long]$script:verifiedIdentities[$parentKey] -eq [long]$parentTime) {
                $key=[string]$node.Pid
                if($script:verifiedIdentities.ContainsKey($key)) { if([long]$script:verifiedIdentities[$key] -ne $nodeTime){continue} }
                else { $script:verifiedIdentities[$key]=$nodeTime; $changed=$true }
            }
        }
    }
    $live=@()
    foreach($key in @($script:verifiedIdentities.Keys)) {
        $pidValue=[int]$key
        try { $now=(Get-Process -Id $pidValue -ErrorAction Stop).StartTime.ToUniversalTime().ToFileTimeUtc(); if([long]$now -eq [long]$script:verifiedIdentities[$key]){$live+=$pidValue} } catch {}
    }
    return $live
}
function Get-VerifiedWindowHandle {
    param([int]$RootId)
    $ids=Get-DescendantProcessIds -ParentIds @($RootId)
    # A non-empty window title is required: a freshly started WinForms installer
    # briefly exposes a title-less bootstrap window (MainWindowHandle non-zero,
    # MainWindowTitle empty) before its real window appears, so a handle alone is
    # not proof the installer window is up. Treating that transient handle as the
    # installer window misclassified a no-window installer as "exited".
    foreach($id in $ids) { try { $p=Get-Process -Id $id -ErrorAction Stop; $p.Refresh(); if($p.MainWindowHandle -ne 0 -and $p.MainWindowTitle){ return $p.MainWindowHandle } } catch {} }
    return [IntPtr]::Zero
}
function Get-RemainingMs {
    if(-not $script:globalDeadline){return 0}
    return [Math]::Max(0,[int][Math]::Ceiling(($script:globalDeadline-[DateTime]::UtcNow).TotalMilliseconds))
}

# ---------------------------------------------------------------------------
# Main flow
# ---------------------------------------------------------------------------

$setupPath = (Resolve-Path $Setup).Path
if (-not (Test-Path $setupPath)) { throw "Setup not found: $setupPath" }
if (-not (Test-Path (Split-Path $setupPath)))   { throw "Setup folder missing" }

# Inno Setup flags: suppress the ordinary wizard, never auto-restart.
$innoArgs = @(
    '/VERYSILENT'
    '/SUPPRESSMSGBOXES'
    '/NORESTART'
    "/DIR=`"$InstallDir`""
    "/LANG=$Lang"
    '/SP-'
)
if ($RevertUac) {
    throw "-RevertUac is not supported: refusing ambiguous explorer.exe tracking."
}

Write-Host ("Launching: {0} {1}" -f $setupPath, ($innoArgs -join ' '))

if ($PSCmdlet.ShouldProcess("setup.exe", "Launch installer and drive it")) {
    # Start-Process accepts an argument array on Windows PowerShell 5.1 and
    # preserves the executable as the tracked process (no explorer handoff).
    $proc = Start-Process -FilePath $setupPath -ArgumentList $innoArgs -WorkingDirectory (Split-Path $setupPath) -PassThru
    $script:verifiedIdentities=@{}
    $script:globalDeadline=[DateTime]::UtcNow.AddMilliseconds($InstallTimeoutMs)

    # 1. Wait for a window to appear.
    $setupWindow=Wait-SetupWindow -Process $proc -TimeoutMs $PageTimeoutMs
    if ($setupWindow -eq [IntPtr]::Zero) {
        Write-Warning "No installer window appeared within $PageTimeoutMs ms."
        if (-not $proc.HasExited) { Write-Result -Status 'process-still-running' -Message 'No installer window appeared; process was left running.' -ExitCode 3 -ProcessIds @($script:verifiedIdentities.Keys | ForEach-Object {[int]$_}) }
        if ($proc.ExitCode -ne 0) { Write-Result -Status 'nonzero' -Message ("Installer exited with code {0} before a window appeared." -f $proc.ExitCode) -ExitCode 5 }
        Write-Result -Status 'no-window' -Message 'Installer exited before a window appeared.' -ExitCode 3
    }

    # 2. Click "Next"/"Install" style buttons by AutomationId as they appear.
    #    Inno Setup's standard button IDs are 'Button1' (Next/Install) etc.,
    #    and the custom ISDone screen drives itself once started.
    $nextIds = @('Button1','NextButton','FinishButton','InstallButton','btnNext','btnInstall','btnFinish')
    $doneSomething = $true
    while ($doneSomething -and (Get-RemainingMs) -gt 0) {
        $doneSomething = $false
        foreach ($id in $nextIds) {
            $proc.Refresh()
            if ($proc.HasExited) { break }
            $window=Get-VerifiedWindowHandle -RootId $proc.Id
            $root = if($window -ne 0){[System.Windows.Automation.AutomationElement]::FromHandle($window)}else{$null}
            if (-not $root) { continue }
            $btn = Find-ElementByAutomationId -Ids @($id) -Root $root -TimeoutMs ([Math]::Min(1000,(Get-RemainingMs)))
            if ($btn) {
                Write-Verbose "Activating button '$id'"
                Invoke-Click $btn
                $doneSomething = $true
                Start-Sleep -Seconds 1
                break
            }
        }

        # DODI quirk: Install button off-screen -> focus window, press UP.
        if ($IsDodi -and -not $doneSomething) {
            $window=Get-VerifiedWindowHandle -RootId $proc.Id
            if ($window -ne 0) {
                $proc.Refresh()
                if ((Get-VerifiedWindowHandle -RootId $proc.Id) -eq $window) {
                    try {
                        [void][System.Windows.Automation.AutomationElement]::FromHandle($window)
                        # Revalidate the verified target before each targeted key.
                        Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Native {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
'@
                        [void][Native]::SetForegroundWindow($window)
                        if ([Native]::GetForegroundWindow() -ne $window) { throw "Installer window did not become foreground." }
                        [void][Native]::PostMessage($window,0x0100,[IntPtr]0x26,[IntPtr]::Zero); [void][Native]::PostMessage($window,0x0101,[IntPtr]0x26,[IntPtr]::Zero)
                        Start-Sleep -Milliseconds 500
                        if ([Native]::GetForegroundWindow() -ne $window) { throw "Installer window lost foreground before Enter." }
                        [void][Native]::PostMessage($window,0x0100,[IntPtr]0x0D,[IntPtr]::Zero); [void][Native]::PostMessage($window,0x0101,[IntPtr]0x0D,[IntPtr]::Zero)
                        $doneSomething = $true
                        Start-Sleep -Seconds 1
                    } catch { Write-Verbose "DODI arrow fallback failed: $_" }
                }
            }
        }

        if (-not $doneSomething) {
            # No clickable control found; give the decompressor time to run.
            Start-Sleep -Seconds 5
        }

        $proc.Refresh()
        if ($proc.HasExited) { break }
    }

    # 3. Wait for setup to finish (decompression is slow) or timeout.
    $proc.Refresh()
    $trackedChildren=@()
    if (-not $proc.HasExited -and (Get-RemainingMs) -gt 0) {
        Write-Host "Waiting for installer to finish (up to $(Get-RemainingMs) ms)..."
        # Poll rather than WaitForExit: this keeps the same monotonic deadline
        # while allowing child-process identity checks to run before the root
        # exits and avoiding PowerShell's process-tree wait behavior.
        while(-not $proc.HasExited -and (Get-RemainingMs) -gt 0) {
            $proc.Refresh()
            Start-Sleep -Milliseconds ([Math]::Min(250,(Get-RemainingMs)))
        }
    }

    do {
        $trackedChildren=Get-DescendantProcessIds -ParentIds @($proc.Id)
        if(@($trackedChildren).Count -eq 0) { break }
        Start-Sleep -Milliseconds 500
    } while((Get-RemainingMs) -gt 0)

    if (-not $proc.HasExited -or @($trackedChildren).Count -gt 0) {
        Write-Warning "Installer still running after timeout; did not kill it."
        Write-Warning "Check the installer window manually before assuming failure."
        Write-Result -Status 'timeout' -Message 'Installer or a verified child process exceeded timeout and was not killed.' -ExitCode 4 -ProcessIds @($script:verifiedIdentities.Keys | ForEach-Object {[int]$_})
    } else {
        Write-Host ("Installer exited with code {0}." -f $proc.ExitCode)
        if ($proc.ExitCode -eq 0) { Write-Result -Status 'exited' -Message 'Installer exited; destination still requires verification.' }
        else { Write-Result -Status 'nonzero' -Message ("Installer exited with code {0}." -f $proc.ExitCode) -ExitCode 5 }
    }
}
