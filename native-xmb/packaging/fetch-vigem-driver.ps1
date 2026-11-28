# Fetches the ViGEmBus installer pinned in vigem-driver.json into <Destination>, verifying it before it is accepted:
# size, SHA-256 AND the Authenticode signature (valid, signed by the pinned signer). Any mismatch deletes the file and
# fails with a non-zero exit, so a package or installer can never carry an unverified driver.
#
#   .\fetch-vigem-driver.ps1 -Destination C:\release\dist\re-ps3\drivers [-CacheDirectory C:\dev\out\vigem-vendor]
#
# The installer is NOT committed (it is a 6 MB vendor binary). The cache directory is for local builds: when it already
# holds the pinned file name, that copy is verified and used instead of downloading. -Offline refuses to download.
param(
    [Parameter(Mandatory)][string]$Destination,
    [string]$CacheDirectory = $env:XMB_VIGEM_CACHE,
    [switch]$Offline
)
$ErrorActionPreference = 'Stop'
$pin = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'vigem-driver.json') -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Path $Destination -Force | Out-Null
$target = Join-Path $Destination $pin.fileName

function Test-Installer([string]$path) {
    $item = Get-Item -LiteralPath $path
    if ($item.Length -ne [int64]$pin.size) { return "size $($item.Length) is not the pinned $($pin.size)" }
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    if ($hash -ne $pin.sha256.ToUpperInvariant()) { return "SHA-256 $hash is not the pinned $($pin.sha256)" }
    $sig = Get-AuthenticodeSignature -LiteralPath $path
    if ($sig.Status -ne 'Valid') { return "Authenticode status is $($sig.Status), not Valid" }
    $subject = $sig.SignerCertificate.GetNameInfo([System.Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false)
    if ($subject -ne $pin.signer) { return "signed by '$subject', not '$($pin.signer)'" }
    return $null
}

$source = $null
if ($CacheDirectory -and (Test-Path -LiteralPath (Join-Path $CacheDirectory $pin.fileName))) {
    $source = Join-Path $CacheDirectory $pin.fileName
    Write-Host "ViGEmBus installer: using the local cache $source"
    Copy-Item -LiteralPath $source -Destination $target -Force
} elseif ($Offline) {
    throw "ViGEmBus installer: $($pin.fileName) is not in the cache and -Offline forbids downloading it."
} else {
    Write-Host "ViGEmBus installer: downloading $($pin.url)"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $pin.url -OutFile $target -UseBasicParsing
}
$problem = Test-Installer $target
if ($problem) {
    Remove-Item -LiteralPath $target -Force -ErrorAction SilentlyContinue
    throw "ViGEmBus installer rejected: $problem."
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'vigem-driver.json') -Destination (Join-Path $Destination 'vigem-driver.json') -Force
Write-Host "ViGEmBus installer verified (size, SHA-256, Authenticode '$($pin.signer)'): $target"
