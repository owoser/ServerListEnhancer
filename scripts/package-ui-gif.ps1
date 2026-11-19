$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$release = Join-Path $root 'dist'
$releaseVersion = Get-Content -LiteralPath (Join-Path $root 'version.json') -Raw | ConvertFrom-Json
$temporaryParent = Join-Path $root 'build'
$temporaryRoot = Join-Path $temporaryParent ('.package-ui-gif-' + [Guid]::NewGuid().ToString('N'))
$artifactFiles = @('ServerListEnhancer.dll', 'ServerListEnhancer.pdb', 'manifest.json')

# Cleanup is restricted to immediate children of the expected directory.
function Remove-PackageItem([string]$Path, [string]$Parent) {
    $target = [IO.Path]::GetFullPath($Path).TrimEnd([char[]]@([char]92, [char]47))
    $expectedParent = [IO.Path]::GetFullPath($Parent).TrimEnd([char[]]@([char]92, [char]47))
    if (-not [string]::Equals([IO.Path]::GetDirectoryName($target), $expectedParent, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove an item outside $expectedParent"
    }
    if (Test-Path -LiteralPath $target) {
        if ((Get-Item -LiteralPath $target -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Refusing to remove a directory link: $target"
        }
        Remove-Item -LiteralPath $target -Recurse -Force
    }
}

# ZIP entries contain only the installable mod. License text lives in the ZIP comment.
function New-PackageArchive([string]$StageRoot, [string]$Destination, [byte[]]$Notice) {
    if ($Notice.Length -gt [UInt16]::MaxValue) { throw 'ZIP notice exceeds the comment limit' }
    $base = [IO.Path]::GetFullPath($StageRoot).TrimEnd([char[]]@([char]92, [char]47)) + [IO.Path]::DirectorySeparatorChar
    $output = [IO.File]::Open($Destination, [IO.FileMode]::CreateNew)
    $archive = $null
    try {
        $archive = [IO.Compression.ZipArchive]::new($output, [IO.Compression.ZipArchiveMode]::Create, $true)
        foreach ($file in (Get-ChildItem -LiteralPath $StageRoot -Recurse -File | Sort-Object FullName)) {
            $name = $file.FullName.Substring($base.Length).Replace([char]92, [char]47)
            $entry = $archive.CreateEntry($name, [IO.Compression.CompressionLevel]::Optimal)
            $entry.LastWriteTime = [DateTimeOffset]'2000-01-01T00:00:00Z'
            $fileStream = [IO.File]::OpenRead($file.FullName)
            try {
                $entryStream = $entry.Open()
                try { $fileStream.CopyTo($entryStream) } finally { $entryStream.Dispose() }
            } finally { $fileStream.Dispose() }
        }
    } finally {
        if ($null -ne $archive) { $archive.Dispose() }
        $output.Dispose()
    }
    # ZipArchive on Windows PowerShell 5.1 has no comment property.
    $zipStream = [IO.File]::Open($Destination, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite)
    try {
        $zipStream.Position = $zipStream.Length - 22
        $reader = [IO.BinaryReader]::new($zipStream, [Text.Encoding]::UTF8, $true)
        try {
            if ($reader.ReadUInt32() -ne 0x06054b50) { throw 'Unexpected ZIP footer' }
        } finally { $reader.Dispose() }
        $zipStream.Position = $zipStream.Length - 2
        $writer = [IO.BinaryWriter]::new($zipStream, [Text.Encoding]::UTF8, $true)
        try {
            $writer.Write([UInt16]$Notice.Length)
            $writer.Write($Notice)
        } finally { $writer.Dispose() }
    } finally { $zipStream.Dispose() }
}

Add-Type -AssemblyName System.IO.Compression
$notice = [IO.File]::ReadAllBytes((Join-Path $root 'THIRD_PARTY_NOTICES.md'))
New-Item -ItemType Directory -Force $temporaryParent | Out-Null
try {
    New-Item -ItemType Directory $temporaryRoot | Out-Null
    $archiveNames = @()
    foreach ($side in @('client', 'server')) {
        $source = Join-Path $release "$side/ServerListEnhancer"
        $manifest = Get-Content -LiteralPath (Join-Path $source 'manifest.json') -Raw | ConvertFrom-Json
        if ($manifest.version -ne $releaseVersion.version -or $manifest.platform -ne $side -or
           $manifest.extraInfo.buildLeviLaminaVersion -ne $releaseVersion.leviLaminaVersion) {
            throw "Stale or mismatched $side manifest; rebuild using version.json before packaging"
        }
        $stageRoot = Join-Path $temporaryRoot $side
        $stage = Join-Path $stageRoot 'ServerListEnhancer'
        New-Item -ItemType Directory -Force $stage | Out-Null
        foreach ($name in $artifactFiles) {
            Copy-Item -LiteralPath (Join-Path $source $name) -Destination $stage
        }
        $config = Join-Path $source 'config'
        if (Test-Path -LiteralPath $config -PathType Container) {
            Copy-Item -LiteralPath $config -Destination (Join-Path $stage 'config') -Recurse
        }
        $archiveName = "ServerListEnhancer-$side-v$($releaseVersion.version).zip"
        New-PackageArchive $stageRoot (Join-Path $temporaryRoot $archiveName) $notice
        $archiveNames += $archiveName
    }
    $lines = foreach ($name in $archiveNames) {
        $hash = (Get-FileHash -LiteralPath (Join-Path $temporaryRoot $name) -Algorithm SHA256).Hash.ToLowerInvariant()
        "$hash  $name"
    }
    $lines | Set-Content -Encoding ascii (Join-Path $temporaryRoot 'SHA256SUMS.txt')

    # Keep the unpacked artifacts in dist; clean their contents only after packaging succeeds.
    foreach ($side in @('client', 'server')) {
        $source = Join-Path $release "$side/ServerListEnhancer"
        foreach ($item in (Get-ChildItem -LiteralPath $source -Force)) {
            if ($item.Name -notin ($artifactFiles + @('config'))) {
                Remove-PackageItem $item.FullName $source
            }
        }
    }
    foreach ($name in ($archiveNames + @('SHA256SUMS.txt'))) {
        Copy-Item -LiteralPath (Join-Path $temporaryRoot $name) -Destination (Join-Path $release $name) -Force
    }
    Remove-PackageItem (Join-Path $release 'ui-gif-20261117') $release
} finally {
    Remove-PackageItem $temporaryRoot $temporaryParent
}
Get-ChildItem -LiteralPath $release -File | Select-Object Name, Length
