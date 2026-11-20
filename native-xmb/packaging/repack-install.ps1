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

function Write-Result {
    param([string]$Status,[string]$Message,[int]$ExitCode = 0)
    [Console]::Out.WriteLine((@{ status=$Status; message=$Message } | ConvertTo-Json -Compress))
    if ($ExitCode -ne 0) { exit $ExitCode }
}

# ---------------------------------------------------------------------------
# UI Automation helpers
# ---------------------------------------------------------------------------

function Get-AutomationRoot {
    [System.Windows.Automation.AutomationElement]::RootElement
}

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
        $c = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $id)
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
        Write-Verbose "No InvokePattern; falling back to keyboard activation"
    }
}

# Wait until the setup process has a visible main window.
function Wait-SetupWindow {
    param([System.Diagnostics.Process]$Process, [int]$TimeoutMs)
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    do {
        $Process.Refresh()
        if ($Process.MainWindowHandle -ne 0) { return $true }
        Start-Sleep -Milliseconds 500
    } while ([DateTime]::UtcNow -lt $deadline)
    return $false
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
$startInfo = New-Object System.Diagnostics.ProcessStartInfo
$startInfo.FileName  = $setupPath
$startInfo.ArgumentList.Clear()
foreach ($argument in $innoArgs) { [void]$startInfo.ArgumentList.Add($argument) }
$startInfo.WorkingDirectory = Split-Path $setupPath
$startInfo.UseShellExecute = $true   # so elevation works if the installer requests it

    if ($RevertUac) {
        throw "-RevertUac is not supported: refusing ambiguous explorer.exe tracking."
}

Write-Host ("Launching: {0} {1}" -f $startInfo.FileName, $startInfo.Arguments)

if ($PSCmdlet.ShouldProcess("setup.exe", "Launch installer and drive it")) {
    $proc = [System.Diagnostics.Process]::Start($startInfo)

    # 1. Wait for a window to appear.
    if (-not (Wait-SetupWindow -Process $proc -TimeoutMs $PageTimeoutMs)) {
        Write-Warning "No installer window appeared within $PageTimeoutMs ms."
        if (-not $proc.HasExited) { Write-Result -Status 'process-still-running' -Message 'No installer window appeared; process was left running.' -ExitCode 3 }
        Write-Result -Status 'failed' -Message 'Installer exited before a window appeared.' -ExitCode 3
    }

    # 2. Click "Next"/"Install" style buttons by AutomationId as they appear.
    #    Inno Setup's standard button IDs are 'Button1' (Next/Install) etc.,
    #    and the custom ISDone screen drives itself once started.
    $nextIds = @('Button1','NextButton','FinishButton','InstallButton','btnNext','btnInstall','btnFinish')
    $doneSomething = $true
    $globalDeadline = [DateTime]::UtcNow.AddMilliseconds($InstallTimeoutMs)

    while ($doneSomething -and [DateTime]::UtcNow -lt $globalDeadline) {
        $doneSomething = $false
        foreach ($id in $nextIds) {
            $proc.Refresh()
            if ($proc.HasExited) { break }
            $root = [System.Windows.Automation.AutomationElement]::FromHandle($proc.MainWindowHandle)
            if (-not $root) { continue }
            $btn = Find-ElementByAutomationId -Ids @($id) -Root $root -TimeoutMs 1000
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
            if ($proc.MainWindowHandle -ne 0) {
                $proc.Refresh()
                if ($proc.MainWindowHandle -ne 0) {
                    try {
                        [void][System.Windows.Automation.AutomationElement]::FromHandle($proc.MainWindowHandle)
                        # Focus the window and send arrow keys / Enter.
                        Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Native {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
'@
                        [void][Native]::SetForegroundWindow($proc.MainWindowHandle)
                        if ([Native]::GetForegroundWindow() -ne $proc.MainWindowHandle) { throw "Installer window did not become foreground." }
                        [System.Windows.Forms.SendKeys]::SendWait('{UP}')
                        Start-Sleep -Milliseconds 500
                        [System.Windows.Forms.SendKeys]::SendWait('{ENTER}')
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
    if (-not $proc.HasExited) {
        Write-Host "Waiting for installer to finish (up to $InstallTimeoutMs ms)..."
        $proc.WaitForExit($InstallTimeoutMs) | Out-Null
    }

    if (-not $proc.HasExited) {
        Write-Warning "Installer still running after timeout; did not kill it."
        Write-Warning "Check the installer window manually before assuming failure."
        Write-Result -Status 'process-still-running' -Message 'Installer exceeded timeout and was not killed.' -ExitCode 4
    } else {
        Write-Host ("Installer exited with code {0}." -f $proc.ExitCode)
        if ($proc.ExitCode -eq 0) { Write-Result -Status 'exited' -Message 'Installer exited; destination still requires verification.' }
        else { Write-Result -Status 'failed' -Message ("Installer exited with code {0}." -f $proc.ExitCode) -ExitCode 5 }
    }
}
