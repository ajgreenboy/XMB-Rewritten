<#
.SYNOPSIS
  Builds every unit-test target exactly the way .github/workflows/tests.yml does, so a broken test-runner
  build is found before a release tag instead of after it.

.DESCRIPTION
  Reads the -DXMB_*_BUILD_TESTS=ON options and the '--target ...' list straight out of tests.yml, configures
  a separate Ninja build directory with them, builds those targets, then runs the runners that need no
  arguments. Run `bun work/ci-guard.ts` first for the static wiring check; this script proves it compiles
  and links with the CI option set (a runner missing a Qt module or a source file fails here).

  Needs the same toolchain as native-xmb/build.ps1 (Visual Studio C++ tools, a Qt 6.8+ MSVC kit).

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File work\ci-config-build.ps1
  powershell -NoProfile -ExecutionPolicy Bypass -File work\ci-config-build.ps1 -SourceRoot C:\dev\xmb-wt\x -OutputDirectory C:\dev\out\ci-x
#>
param(
    [string]$SourceRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$OutputDirectory,
    [string]$QtRoot = $env:QTDIR
)
$ErrorActionPreference = 'Stop'
$yml = Get-Content (Join-Path $SourceRoot '.github\workflows\tests.yml') -Raw
$opts = ([regex]::Matches($yml, '-DXMB_[A-Z0-9_]+_BUILD_TESTS=ON') | ForEach-Object { $_.Value } | Sort-Object -Unique) -join ' '
$m = [regex]::Match($yml, '--target `\r?\n([\s\S]*?)\r?\n\s*if \(\$LASTEXITCODE\)')
if (-not $m.Success) { throw "No '--target' build step found in tests.yml." }
$targets = ($m.Groups[1].Value -replace '`', ' ' -split '\s+' | Where-Object { $_ }) -join ' '
if (-not $QtRoot) {
    $qtBase = Join-Path ([IO.Path]::GetPathRoot($SourceRoot)) 'Qt'
    $kits = @(Get-ChildItem $qtBase -Directory | Sort-Object Name -Descending | ForEach-Object { Join-Path $_.FullName 'msvc2022_64' } | Where-Object { Test-Path (Join-Path $_ 'lib\cmake\Qt6\Qt6Config.cmake') })
    if (-not $kits.Count) { throw 'Pass -QtRoot pointing to a Qt 6.8+ MSVC kit.' }
    $QtRoot = $kits[0]
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
if (-not $vs) { throw 'Visual Studio C++ Build Tools are required.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ninja = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
$out = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $SourceRoot 'out\ci-config' }
$src = Join-Path $SourceRoot 'native-xmb'

cmd /d /c ('call "' + $vcvars + '" >nul && "' + $cmake + '" -S "' + $src + '" -B "' + $out + '" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="' + $QtRoot + '" -DCMAKE_MAKE_PROGRAM="' + $ninja + '" ' + $opts)
if ($LASTEXITCODE) { throw 'CI-config CMake configuration failed.' }
cmd /d /c ('call "' + $vcvars + '" >nul && "' + $cmake + '" --build "' + $out + '" --parallel --target ' + $targets)
if ($LASTEXITCODE) { throw 'CI-config test build failed.' }
Write-Host "ci-config-build: built $(@($targets -split ' ').Count) targets into $out"

# Runners that take no arguments are run here (QT_QPA_PLATFORM=offscreen only for these children, never
# exported to the caller: a leaked offscreen platform breaks the render gate's captures).
$old = @{ P = $env:QT_QPA_PLATFORM; Q = $env:QT_PLUGIN_PATH; PATH = $env:PATH }
$env:QT_QPA_PLATFORM = 'offscreen'; $env:QT_PLUGIN_PATH = Join-Path $QtRoot 'plugins'; $env:PATH = (Join-Path $QtRoot 'bin') + ';' + $env:PATH
$failed = @()
try {
    foreach ($n in 'xmb-jellyfin-playback-tests', 'xmb-jellyfin-player-tests', 'xmb-jellyfin-music-tests', 'xmb-frame-pacer-tests', 'xmb-osk-paste-tests', 'xmb-steam-vdf-tests', 'xmb-steam-switch-tests') {
        $exe = Join-Path $out "$n.exe"
        if (-not (Test-Path $exe)) { continue }
        & $exe | Out-Null
        if ($LASTEXITCODE) { $failed += $n; Write-Host "FAIL $n (exit $LASTEXITCODE)" } else { Write-Host "ok   $n" }
    }
} finally {
    $env:QT_QPA_PLATFORM = $old.P; $env:QT_PLUGIN_PATH = $old.Q; $env:PATH = $old.PATH
}
if ($failed.Count) { throw "Failing runners: $($failed -join ', ')" }
Write-Host 'ci-config-build: ok'
