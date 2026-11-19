param(
    [ValidateSet('arm64-v8a')][string]$Abi = 'arm64-v8a',
    [string]$SdkRoot = '',
    [string]$NdkHome = '',
    [string]$NdkVersion = '28.2.13676358',
    [string]$CMake = 'cmake',
    [string]$HostCompiler = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
$build = Join-Path $root 'build/android'
$hostBuild = Join-Path $build 'host'
$targetBuild = Join-Path $build $Abi
$output = Join-Path $root 'dist/android'
function Invoke-Checked([string]$Exe, [string[]]$Arguments) {
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed with exit code $LASTEXITCODE" }
}
if (-not $SdkRoot) {
    foreach ($candidate in @($env:ANDROID_HOME, $env:ANDROID_SDK_ROOT, (Join-Path $env:LOCALAPPDATA 'Android/Sdk'))) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Container)) { $SdkRoot=$candidate; break }
    }
}
if (-not $NdkHome) {
    if ($env:ANDROID_NDK_HOME) { $NdkHome=$env:ANDROID_NDK_HOME }
    else {
        $candidates = @((Join-Path $build "../android-sdk/ndk/$NdkVersion"))
        if ($SdkRoot) { $candidates += Join-Path $SdkRoot "ndk/$NdkVersion" }
        foreach ($candidate in $candidates) {
            if (Test-Path -LiteralPath $candidate -PathType Container) { $NdkHome=$candidate; break }
        }
    }
}
if (-not $NdkHome) { throw "NDK $NdkVersion not found. Install the launcher-matching NDK or specify -NdkHome; do not automatically select a newer C++ runtime." }
$NdkHome = (Resolve-Path -LiteralPath $NdkHome).Path
$toolchain = Join-Path $NdkHome 'build/cmake/android.toolchain.cmake'
if (-not (Test-Path -LiteralPath $toolchain -PathType Leaf)) { throw 'Invalid Android NDK toolchain path' }
$ninja = (Get-Command ninja -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
if (-not $HostCompiler) {
    $gcc = Get-Command g++ -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($gcc) { $HostCompiler=$gcc.Source }
}
$javaBuild = Join-Path $build "java"
$javaClasses = Join-Path $javaBuild "classes"
$javaDex = Join-Path $javaBuild "dex"
if (-not $SdkRoot) { throw "Android SDK required to compile the home-screen view (android.jar and d8)." }
$androidJar = Get-ChildItem -LiteralPath (Join-Path $SdkRoot "platforms") -Filter android.jar -Recurse -File | Sort-Object FullName -Descending | Select-Object -First 1
$d8 = Get-ChildItem -LiteralPath (Join-Path $SdkRoot "build-tools") -Filter d8.bat -Recurse -File | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $androidJar -or -not $d8) { throw "Install Android SDK Platform and Build Tools (android.jar/d8 missing)." }
New-Item -ItemType Directory -Force $javaClasses, $javaDex | Out-Null
Invoke-Checked "javac" @("-encoding", "UTF-8", "-source", "8", "-target", "8", "-classpath", $androidJar.FullName, "-d", $javaClasses, (Join-Path $root "android/java/cn/owoser/sle/HomeOverlay.java"))
$classFiles = @(Get-ChildItem -LiteralPath $javaClasses -Filter *.class -Recurse -File | ForEach-Object { $_.FullName })
Invoke-Checked $d8.FullName (@("--release", "--min-api", "28", "--lib", $androidJar.FullName, "--output", $javaDex) + $classFiles)
$depsCache = Join-Path $build 'deps'
$depFlags = @()
$allCached = $true
foreach ($pair in @(
    @('PRELOADER_ANDROID', 'preloader_android-src'),
    @('NLOHMANN_JSON', 'nlohmann_json-src'),
    @('BOOST_PFR', 'boost_pfr-src'),
    @('MAGIC_ENUM', 'magic_enum-src'),
    @('FMT', 'fmt-src'),
    @('LUNASVG', 'lunasvg-src')
)) {
    $cachedSrc = Join-Path $depsCache $pair[1]
    if (-not (Test-Path -LiteralPath (Join-Path $cachedSrc 'CMakeLists.txt') -PathType Leaf)) {
        $cachedSrc = Join-Path $targetBuild "_deps/$($pair[1])"
    }
    if (Test-Path -LiteralPath (Join-Path $cachedSrc 'CMakeLists.txt') -PathType Leaf) {
        $depFlags += "-DFETCHCONTENT_SOURCE_DIR_$($pair[0])=$($cachedSrc.Replace([char]92,[char]47))"
    } else {
        $allCached = $false
    }
}
if ($allCached) { $depFlags += '-DFETCHCONTENT_FULLY_DISCONNECTED=ON' }
$hostOptions=@('-S', (Join-Path $root 'android'), '-B', $hostBuild, '-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$ninja", '-DCMAKE_BUILD_TYPE=Release') + $depFlags
if ($HostCompiler) { $hostOptions += "-DCMAKE_CXX_COMPILER=$HostCompiler" }
Invoke-Checked $CMake $hostOptions
Invoke-Checked $CMake @('--build', $hostBuild, '--target', 'sle_generate_config', 'sle_android_model_test', 'sle_android_touch_test', 'sle_android_ime_test', '--parallel', '4')
Invoke-Checked 'ctest' @('--test-dir', $hostBuild, '--output-on-failure')
Invoke-Checked $CMake (@('--fresh', '-S', (Join-Path $root 'android'), '-B', $targetBuild, '-G', 'Ninja',
    "-DCMAKE_MAKE_PROGRAM=$ninja", "-DCMAKE_TOOLCHAIN_FILE=$toolchain", "-DANDROID_ABI=$Abi", '-DANDROID_PLATFORM=android-28',
    '-DANDROID_STL=c++_shared', '-DCMAKE_BUILD_TYPE=Release', "-DLEVI_PACKAGE_CONFIG_DIR=$hostBuild/generated-config") + $depFlags)
Invoke-Checked $CMake @('--build', $targetBuild, '--target', 'server_list_enhancer', '--parallel', '4')
$llvm = Join-Path $NdkHome 'toolchains/llvm/prebuilt/windows-x86_64/bin'
$library = Join-Path $targetBuild 'libserver_list_enhancer.so'
Invoke-Checked (Join-Path $llvm 'llvm-strip.exe') @('--strip-unneeded', $library)
$elf = & (Join-Path $llvm 'llvm-readelf.exe') -h -l -d --dyn-syms --wide $library
if ($LASTEXITCODE -ne 0) { throw 'ELF inspection failed' }
$elf | Set-Content -Encoding utf8 (Join-Path $targetBuild 'elf-validation.txt')
if (($elf -join "$([char]10)") -notmatch 'AArch64' -or ($elf -join "$([char]10)") -notmatch 'GLOBAL\s+DEFAULT\s+\d+\s+PLGetModRegistration') { throw 'Not an Android ARM64 mod with PLGetModRegistration' }
foreach ($line in $elf) { if ($line -match '^\s*LOAD\s+' -and $line -notmatch '0x4000\s*$|0x10000\s*$') { throw "Insufficient ELF page alignment: $line" } }
if (($elf -join "$([char]10)") -notmatch 'Shared library: \[libpreloader\.so\]') { throw 'Missing libpreloader.so DT_NEEDED dependency' }
$imports = & (Join-Path $llvm 'llvm-nm.exe') --dynamic --undefined-only --demangle $library
if ($LASTEXITCODE -ne 0) { throw 'ELF symbol inspection failed' }
if (($imports -join "$([char]10)") -match 'sle::') { throw 'Unresolved project symbol in Android library' }
if (($imports -join "$([char]10)") -match 'std::__ndk1::__hash_memory') { throw 'C++ runtime mismatch: rebuild with launcher-matching NDK 28.2, not NDK 30' }
New-Item -ItemType Directory -Force $output | Out-Null
$stage = Join-Path $build ('.package-' + [guid]::NewGuid().ToString('N'))
$mod = Join-Path $stage 'server_list_enhancer'
try {
    New-Item -ItemType Directory -Force "$mod/config", "$mod/resources" | Out-Null
    Copy-Item -LiteralPath $library -Destination $mod
    Copy-Item -LiteralPath (Join-Path $javaDex "classes.dex") -Destination "$mod/ui.dex"
    Copy-Item -LiteralPath (Join-Path $targetBuild 'manifest.json') -Destination $mod
    Copy-Item -LiteralPath (Join-Path $hostBuild 'generated-config/config.json'), (Join-Path $hostBuild 'generated-config/config.schema.json') -Destination "$mod/config"
    Copy-Item -LiteralPath (Join-Path $root 'resources/logo/default.png') -Destination "$mod/resources/logo.png"
    New-Item -ItemType Directory -Force "$mod/resources/minecraft_resource_packs" | Out-Null
    Copy-Item -LiteralPath (Join-Path $root 'android/resources/minecraft_resource_packs/sle_home') -Destination "$mod/resources/minecraft_resource_packs/sle_home" -Recurse
    Copy-Item -LiteralPath (Join-Path $root 'docs/android.md') -Destination "$mod/README.md"
    Copy-Item -LiteralPath (Join-Path $root 'THIRD_PARTY_NOTICES.md') -Destination $mod
    function Resolve-DepFile([string]$depDir, [string]$relPath) {
        $fromCache = Join-Path (Join-Path $depsCache $depDir) $relPath
        if (Test-Path -LiteralPath $fromCache -PathType Leaf) { return $fromCache }
        return Join-Path (Join-Path $targetBuild "_deps/$depDir") $relPath
    }
    $fmtLicense = Resolve-DepFile 'fmt-src' 'LICENSE'
    if (Test-Path -LiteralPath $fmtLicense) { Copy-Item -LiteralPath $fmtLicense -Destination "$mod/FMT-LICENSE.txt" }
    $jsonLicense = Resolve-DepFile 'nlohmann_json-src' 'LICENSE.MIT'
    if (Test-Path -LiteralPath $jsonLicense) { Copy-Item -LiteralPath $jsonLicense -Destination "$mod/JSON-LICENSE.txt" }
    Copy-Item -LiteralPath (Resolve-DepFile 'boost_pfr-src' 'LICENSE_1_0.txt') -Destination "$mod/BOOST-LICENSE.txt"
    Copy-Item -LiteralPath (Resolve-DepFile 'magic_enum-src' 'LICENSE') -Destination "$mod/MAGIC-ENUM-LICENSE.txt"
    $name = "ServerListEnhancer-1.0.0-$Abi.levipack"
    $temporaryArchive = "$stage.levipack"
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::Open($temporaryArchive, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($file in Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName) {
            $relative = $file.FullName.Substring($stage.Length + 1).Replace([char]92,[char]47)
            $entry = $archive.CreateEntry($relative, [IO.Compression.CompressionLevel]::Optimal)
            $entry.LastWriteTime = [DateTimeOffset]::new(2000,1,1,0,0,0,[TimeSpan]::Zero)
            $input = $file.OpenRead(); $stream = $entry.Open()
            try { $input.CopyTo($stream) } finally { $stream.Dispose(); $input.Dispose() }
        }
    } finally { $archive.Dispose() }
    $check = [IO.Compression.ZipFile]::OpenRead($temporaryArchive)
    try {
        $names = @($check.Entries | ForEach-Object { $_.FullName })
        foreach ($required in @('manifest.json','libserver_list_enhancer.so','ui.dex','config/config.json','config/config.schema.json','resources/logo.png','resources/minecraft_resource_packs/sle_home/manifest.json','resources/minecraft_resource_packs/sle_home/ui/start_screen.json')) {
            if ($names -cnotcontains "server_list_enhancer/$required") { throw "Missing package entry: $required" }
        }
        if ($names | Where-Object { $_ -notlike 'server_list_enhancer/*' -or $_ -match '\\|(^|/)\.\.(/|$)|servers\.json|cache/' }) { throw 'Invalid package path or user data included' }
    } finally { $check.Dispose() }
    $destination = Join-Path $output $name
    Copy-Item -LiteralPath $temporaryArchive -Destination $destination -Force
    $hash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $name" | Set-Content -Encoding ascii (Join-Path $output 'SHA256SUMS.txt')
    $head = & git -C $root rev-parse HEAD
    $dirty = [bool](& git -C $root status --porcelain)
    [ordered]@{
        package=$name; sha256=$hash; abi=$Abi; androidApi=28; sdk='Preloader for LeviLauncher 1.6.0';
        sdkCommit='5d108da350ce3e7f13f51415f81e65344ecd5d14'; sourceCommit=$head; sourceDirty=$dirty;
        ndk=([IO.File]::ReadAllText((Join-Path $NdkHome 'source.properties')) -split [char]10 | Where-Object { $_ -like 'Pkg.Revision*' } | ForEach-Object { ($_ -split '=')[1].Trim() });
        librarySha256=(Get-FileHash -LiteralPath $library -Algorithm SHA256).Hash.ToLowerInvariant()
    } | ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $output 'build-info.json')
    Get-ChildItem -LiteralPath $output -File | Select-Object Name,Length
} finally {
    # Only remove this invocation's immediate child of the known build directory.
    $fullStage = [IO.Path]::GetFullPath($stage)
    if ([IO.Path]::GetDirectoryName($fullStage) -ne [IO.Path]::GetFullPath($build) -or [IO.Path]::GetFileName($fullStage) -notmatch '^\.package-[a-f0-9]{32}$') { throw 'Refusing cleanup outside Android build staging' }
    if (Test-Path -LiteralPath $fullStage) {
        if ((Get-Item -LiteralPath $fullStage).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Refusing to delete a linked staging directory' }
        Remove-Item -LiteralPath $fullStage -Recurse -Force
    }
    if (Test-Path -LiteralPath "$fullStage.levipack") { Remove-Item -LiteralPath "$fullStage.levipack" -Force }
}
