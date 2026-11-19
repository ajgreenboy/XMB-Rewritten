$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build-metadata.ps1')

# Shadow the cmdlet to prove the implementation does not depend on an
# optional PowerShell hashing command being available in a CMake POST_BUILD shell.
function Get-FileHash { throw 'Get-FileHash must not be called by native metadata hashing.' }

$path=Join-Path $env:TEMP ('xmb-hash-test-{0}.bin' -f [Guid]::NewGuid())
try {
    [IO.File]::WriteAllBytes($path,[byte[]](0,1,2,3,255))
    $actual=Get-NativeSha256 -LiteralPath $path
    # The expected value is calculated from the exact bytes above; retain a
    # separate known-value assertion so accidental algorithm/format changes fail.
    if($actual -ne 'FF5D8507B6A72BEE2DEBCE2C0054798DEACCDC5D8A1B945B6280CE8AA9CBA52E') {
        throw "Unexpected SHA-256: $actual"
    }
    if($actual -cnotmatch '^[0-9A-F]{64}$') { throw "Hash is not uppercase hexadecimal: $actual" }
    Write-Host "PASS build metadata SHA-256: $actual"
}
finally {
    Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
}
