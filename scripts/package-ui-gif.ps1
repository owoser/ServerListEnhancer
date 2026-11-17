$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$release = Join-Path $root 'dist/ui-gif-20261117'
New-Item -ItemType Directory -Force $release | Out-Null
foreach ($side in @('client','server')) {
    $source = Join-Path $root "dist/$side/ServerListEnhancer"
    $stage = Join-Path $release "$side/ServerListEnhancer"
    New-Item -ItemType Directory -Force "$stage/docs" | Out-Null
    Copy-Item -Force "$source/ServerListEnhancer.dll", "$source/manifest.json" $stage
    Copy-Item -Force "$root/THIRD_PARTY_NOTICES.md" $stage
    Copy-Item -Force "$root/docs/discovery-images.md", "$root/docs/validation-ui-gif.md", "$root/docs/mcapi-request.md" "$stage/docs/"
    @'
# ServerListEnhancer — UI / GIF / UDP iteration (2026-11-17)

Windows x64, Minecraft Bedrock 1.26.60.05 + ordinary LeviLamina 26.60.7.
Client and server DLLs are different; never interchange them. Manifest stays at 1.0.0.

- Client: exit Minecraft, merge this ServerListEnhancer folder into its mods folder.
- Server: stop the server, merge this folder into mods (or the existing plugins mapping).
- Preserve all existing servers.json, config and cache. Never delete the old directory.
- This package intentionally contains no live config/config.json or server-list data.
- Server first start creates defaults; config.example.json is reference only.
- No DearOreUI DLL, replacement LL, game assets or user GIF downloads are included.
- Server DLL updates require restart, not hot unload. Configuration reload: /sle reload.
- UDP echo is opt-in (udpProbePort defaults to 0). UDP no response does not imply offline.
- Clicking a row selects details; direct connect is not implemented with this SDK.
- Chinese paste works; full IME composition is not implemented.

See [configuration and image budgets](<docs/discovery-images.md>),
[validation and limitations](<docs/validation-ui-gif.md>) and
[third-party notices](<THIRD_PARTY_NOTICES.md>).

Source: https://github.com/owoser/ServerListEnhancer
Build provenance and publication history are available in the source repository.
'@ | Set-Content -Encoding utf8 "$stage/README.md"
    if ($side -eq 'server') {
        @'
{
  "version": 1,
  "enabled": true,
  "serverId": "my-server",
  "logoId": "default",
  "logoUrl": "",
  "logoData": "",
  "introText": "欢迎来到本服务器",
  "introImageId": "default",
  "introImageUrl": "",
  "udpProbePort": 0,
  "logLevel": 4
}
'@ | Set-Content -Encoding utf8 "$stage/config.example.json"
    }
    $entries = Get-ChildItem -Recurse -File $stage
    if ($entries | Where-Object { $_.Name -eq 'servers.json' -or $_.FullName.Replace([char]92,[char]47).Contains('/config/') -or $_.Name -eq 'DearOreUI.dll' }) {
        throw 'Refusing to ship user data, live configuration or DearOreUI'
    }
    Compress-Archive -Path $stage -DestinationPath (Join-Path $release "ServerListEnhancer-$side-ui-gif-20261117.zip") -Force
}
$files = @('ServerListEnhancer-client-ui-gif-20261117.zip', 'ServerListEnhancer-server-ui-gif-20261117.zip', 'client/ServerListEnhancer/ServerListEnhancer.dll', 'server/ServerListEnhancer/ServerListEnhancer.dll')
$lines = foreach ($file in $files) { $hash = (Get-FileHash (Join-Path $release $file) -Algorithm SHA256).Hash.ToLowerInvariant(); "$hash  $file" }
$lines | Set-Content -Encoding ascii (Join-Path $release 'SHA256SUMS.txt')
Get-ChildItem $release -File | Select-Object Name,Length
