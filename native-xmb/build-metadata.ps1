function Get-NativeSha256 {
    param([Parameter(Mandatory=$true)][string]$LiteralPath)
    $sha=[Security.Cryptography.SHA256]::Create()
    $stream=$null
    try {
        $stream=[IO.File]::OpenRead($LiteralPath)
        return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-','')
    }
    finally {
        if($stream){$stream.Dispose()}
        $sha.Dispose()
    }
}

function Get-NativeSourceFingerprint {
    param([Parameter(Mandatory=$true)][string]$ProjectRoot)
    $nativeRoot=Join-Path $ProjectRoot 'native-xmb'
    $files=@(Get-ChildItem (Join-Path $nativeRoot 'src') -File -Recurse | Where-Object {$_.Extension -in @('.cpp','.h')})
    $files+=@(Get-Item (Join-Path $nativeRoot 'CMakeLists.txt'),(Join-Path $nativeRoot 'build.ps1'),(Join-Path $nativeRoot 'build-metadata.ps1'),(Join-Path $nativeRoot 'write-build-info.ps1'))
    $records=@($files | Sort-Object FullName | ForEach-Object {
        $_.FullName.Substring($nativeRoot.Length).Replace('\','/')+' '+(Get-NativeSha256 -LiteralPath $_.FullName)
    })
    $sha=[Security.Cryptography.SHA256]::Create()
    try { ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes(($records -join "`n"))))).Replace('-','') }
    finally { $sha.Dispose() }
}

# The commit the source tree is based on, plus whether the working tree has
# uncommitted changes. The fingerprint hashes the actual files (dirty or not), so
# recording a clean commit hash next to it would overstate what was built; the
# Dirty flag makes that honest. Returns $null revision when there is no git.
function Get-NativeSourceRevision {
    param([Parameter(Mandatory=$true)][string]$ProjectRoot)
    $revision=$null
    try { $revision=(& git -C $ProjectRoot rev-parse HEAD 2>$null | Select-Object -First 1) } catch { $revision=$null }
    $dirty=$false
    if($revision) {
        try {
            $lines=@(& git -C $ProjectRoot status --porcelain --untracked-files=all 2>$null)
            $dirty=($lines.Count -gt 0)
        } catch { $dirty=$false }
    }
    [pscustomobject]@{ Revision=$revision; Dirty=$dirty }
}
