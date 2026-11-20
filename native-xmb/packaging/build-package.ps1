param(
    [string]$OutputDirectory,
    [string]$PackageDirectory,
    [switch]$Zip
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)   # native-xmb/packaging -> repo root
if(-not $OutputDirectory) { $OutputDirectory = Join-Path $repoRoot 'out\integration' }
if(-not $PackageDirectory) { $PackageDirectory = Join-Path $repoRoot 'dist\re-ps3' }

$exe = Join-Path $OutputDirectory 're-ps3-native.exe'
if(-not (Test-Path $exe)) { throw "No build at '$OutputDirectory' (re-ps3-native.exe missing). Run native-xmb\build.ps1 first." }

# Verify the build metadata still matches the executable and the sources before
# copying anything. A direct `cmake --build`/Ninja relink refreshes build-info.json
# (see CMakeLists.txt), but a stale or hand-edited file must never be packaged.
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'build-metadata.ps1')
$infoPath = Join-Path $OutputDirectory 'build-info.json'
if(-not (Test-Path -LiteralPath $infoPath -PathType Leaf)) {
    throw "No build-info.json at '$OutputDirectory'. Run native-xmb\build.ps1 first."
}
$info = $null
try {
    $info = Get-Content -LiteralPath $infoPath -Raw | ConvertFrom-Json
} catch {
    throw "build-info.json at '$infoPath' is not valid JSON. Rebuild with native-xmb\build.ps1 before packaging."
}
if(-not $info -or $info.schema -ne 'xmb-build/1') {
    throw "build-info.json at '$infoPath' has an unexpected schema (expected 'xmb-build/1'). Rebuild with native-xmb\build.ps1 before packaging."
}
if($info.executableSha256 -ne (Get-NativeSha256 -LiteralPath $exe)) {
    throw "build-info.json is stale: its executableSha256 does not match $exe. Rebuild with native-xmb\build.ps1 before packaging."
}
if($info.sourceFingerprint -ne (Get-NativeSourceFingerprint -ProjectRoot $repoRoot)) {
    throw "build-info.json is stale: its sourceFingerprint does not match the current sources. Rebuild with native-xmb\build.ps1 before packaging."
}

Write-Host "Packaging '$OutputDirectory' -> '$PackageDirectory'"
New-Item -ItemType Directory -Path $PackageDirectory -Force | Out-Null

# Copy the portable runtime (exes, Qt DLLs, plugin folders, pdb) but skip CMake build intermediates.
# robocopy exit codes 0-7 are success; 8+ are errors.
robocopy $OutputDirectory $PackageDirectory /E `
    /XD CMakeFiles re-ps3-native_autogen xmb-manager_autogen xmb-reference-frames_autogen `
    /XF build.ninja .ninja_deps .ninja_log CMakeCache.txt cmake_install.cmake *.ilk | Out-Null
if($LASTEXITCODE -ge 8) { throw "robocopy failed (exit code $LASTEXITCODE)." }
# 1-7 mean files were copied; clear it so a caller that exits with $LASTEXITCODE (GitHub Actions' pwsh
# steps do) doesn't report the copy as a failure.
$global:LASTEXITCODE = 0

# The launcher, watchdog, autostart and docs.
foreach($name in 'run-kiosk.ps1','watchdog.ps1','kiosk-launch.vbs','apply-update.ps1','install-autostart.ps1','repack-install.ps1','README.md','xmb-config.example.json') {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $PackageDirectory -Force
}

Write-Host ''
Write-Host 'Packaged. Contents:'
Get-ChildItem -LiteralPath $PackageDirectory -File | Select-Object Name, @{n='KB';e={[math]::Round($_.Length/1KB)}} | Format-Table -AutoSize

if($Zip) {
    $zipPath = "$PackageDirectory.zip"
    if(Test-Path $zipPath) { Remove-Item $zipPath -Force }
    Compress-Archive -Path (Join-Path $PackageDirectory '*') -DestinationPath $zipPath
    Write-Host "Zipped to $zipPath"
}
Write-Host ''
Write-Host 'Next: copy the folder to the HTPC, write xmb-config.json (firmware + data paths),'
Write-Host 'then run install-autostart.ps1 to register kiosk autostart. See packaging/README.md.'
