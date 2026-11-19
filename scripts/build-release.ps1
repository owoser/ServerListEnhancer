param([switch]$ValidateOnly)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
$release = Get-Content -LiteralPath (Join-Path $root 'version.json') -Raw | ConvertFrom-Json
$parts = $release.leviLaminaVersion -split '\.'
if ($parts.Count -ne 3 -or $release.leviLaminaVersion -notmatch '^\d+\.\d+\.\d+$' -or
    ($release.revision -isnot [int] -and $release.revision -isnot [long]) -or $release.revision -lt 1 -or $release.revision -gt 999) {
    throw 'Expected a three-part LL version and integer release revision 1..999'
}
$expectedVersion = "$($parts[0]).$($parts[1]).$([int]$parts[2] * 1000 + $release.revision)"
if ($release.version -ne $expectedVersion) {
    throw 'version.json does not follow the approved LL patch * 1000 + revision numbering'
}
function Invoke-Checked([string]$Exe, [string[]]$Arguments) {
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed with exit code $LASTEXITCODE" }
}
# Validate with the installed lip parser, including Scriban template expansion.
$viewText = & lip pkg view $root
if ($LASTEXITCODE -ne 0) { throw 'lip.toml validation failed' }
$manifest = ($viewText -join "`n") | ConvertFrom-Json -AsHashtable
if ($manifest.manifest.version -ne $release.version) { throw 'lip.toml version mismatch' }
$variants = @($manifest.manifest.variants.'')
if ($variants.Count -ne 2) { throw 'Expected exactly the Windows server and client lip entries' }
foreach ($side in @('client', 'server')) {
    $entry = @($variants | Where-Object { $_.side -eq $side -and $_.platform -eq 'win-x64' })
    if ($entry.Count -ne 1 -or $entry[0].deps.'github.com/LiteLDev/LeviLamina' -ne $release.leviLaminaVersion) {
        throw "Missing or mismatched $side lip dependency"
    }
    $expectedUrl = "https://github.com/owoser/ServerListEnhancer/releases/download/v$($release.version)/ServerListEnhancer-$side-v$($release.version).zip"
    if ($entry[0].assets[0].urls[0] -ne $expectedUrl) { throw "Unexpected $side asset URL" }
}
Write-Host "Release metadata validated: $($release.version), LL $($release.leviLaminaVersion)"
if ($ValidateOnly) { return }
# This script never changes a version, creates a tag, pushes, or publishes a release.
foreach ($side in @('client', 'server')) {
    Invoke-Checked 'xmake' @('f', '-P', $root, "--target_type=$side", "--ll_version=$($release.leviLaminaVersion)", '-y')
    Invoke-Checked 'xmake' @('-P', $root, '-y')
    $built = Join-Path $root 'bin/ServerListEnhancer'
    $stage = Join-Path $root "dist/$side/ServerListEnhancer"
    $modManifest = Get-Content -LiteralPath (Join-Path $built 'manifest.json') -Raw | ConvertFrom-Json
    if ($modManifest.version -ne $release.version -or $modManifest.platform -ne $side -or
        $modManifest.extraInfo.buildLeviLaminaVersion -ne $release.leviLaminaVersion) {
        throw "Mismatched $side build manifest"
    }
    New-Item -ItemType Directory -Force $stage | Out-Null
    foreach ($name in @('ServerListEnhancer.dll', 'ServerListEnhancer.pdb', 'manifest.json')) {
        Copy-Item -LiteralPath (Join-Path $built $name) -Destination (Join-Path $stage $name) -Force
    }
}
Invoke-Checked 'pwsh' @('-NoProfile', '-File', (Join-Path $PSScriptRoot 'package-ui-gif.ps1'))
Invoke-Checked 'pwsh' @('-NoProfile', '-File', (Join-Path $PSScriptRoot 'package-android.ps1'))
Add-Type -AssemblyName System.IO.Compression.FileSystem
$androidName = "ServerListEnhancer-$($release.version)-arm64-v8a.levipack"
$androidPath = Join-Path $root "dist/android/$androidName"
$archive = [IO.Compression.ZipFile]::OpenRead($androidPath)
try {
    $entry = $archive.GetEntry('server_list_enhancer/manifest.json')
    if (-not $entry) { throw 'Android manifest missing' }
    $reader = [IO.StreamReader]::new($entry.Open())
    try { $androidManifest = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
    if ($androidManifest.version -ne $release.version -or
        $androidManifest.extraInfo.buildLeviLaminaVersion -ne $release.leviLaminaVersion) {
        throw 'Android package version mismatch'
    }
} finally { $archive.Dispose() }
$artifacts = @(
    "ServerListEnhancer-client-v$($release.version).zip",
    "ServerListEnhancer-server-v$($release.version).zip",
    "android/$androidName"
)
$checksums = foreach ($relative in $artifacts) {
    $file = Join-Path $root "dist/$relative"
    $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $([IO.Path]::GetFileName($file))"
}
$checksums | Set-Content -LiteralPath (Join-Path $root 'dist/SHA256SUMS.txt') -Encoding ascii
Write-Host 'Release artifacts built and validated. Publishing requires a separate explicit user request.'
$checksums
