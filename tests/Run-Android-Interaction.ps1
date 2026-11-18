param([string]$SdkRoot = "$env:LOCALAPPDATA/Android/Sdk")
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path $PSScriptRoot -Parent
$classes = Join-Path $root 'build/android/interaction-tests'
$androidJar = Get-ChildItem -LiteralPath (Join-Path $SdkRoot 'platforms') -Filter android.jar -Recurse -File | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $androidJar) { throw 'Android SDK android.jar is required' }
New-Item -ItemType Directory -Force $classes | Out-Null
& cmake --build (Join-Path $root 'build/android/host') --target sle_android_model_test sle_android_touch_test --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Android host tests failed to build' }
& ctest --test-dir (Join-Path $root 'build/android/host') --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Android host tests failed' }
& javac -encoding UTF-8 -source 8 -target 8 -classpath $androidJar.FullName -d $classes (Join-Path $root 'android/java/cn/owoser/sle/HomeOverlay.java') (Join-Path $PSScriptRoot 'HomeEntryInteractionTest.java')
if ($LASTEXITCODE -ne 0) { throw 'Android entry tests failed to compile' }
# Only pure nested helper classes are loaded: no Android runtime or mocked GUI is involved.
& java -classpath $classes cn.owoser.sle.HomeEntryInteractionTest
if ($LASTEXITCODE -ne 0) { throw 'Android entry tests failed' }
$pack = Join-Path $root 'android/resources/minecraft_resource_packs/sle_home'
$ui = Get-Content -LiteralPath (Join-Path $pack 'ui/start_screen.json') -Raw | ConvertFrom-Json
$entry = $ui.friendsdrawer_button_panel.modifications[0].value[0].sle_home_entry
if (($entry.size -join ',') -ne '50,24') { throw 'Entry dimensions must match the geometry detector' }
$button = $entry.controls[0].'sle_home_servers_button@common_buttons.light_text_button'
if ($button.'$pressed_button_name' -ne 'button.sle_home_entry') { throw 'Native button action changed' }
$markers = @($entry.controls[1].sle_entry_top_left, $entry.controls[2].sle_entry_bottom_right)
$expected = @('198,190,202', '190,202,198')
$anchors = @('top_left', 'bottom_right')
for ($index = 0; $index -lt 2; ++$index) {
    $marker = $markers[$index]
    $rgb = @($marker.color[0..2] | ForEach-Object { [int][Math]::Round($_ * 255) }) -join ','
    if (($marker.size -join ',') -ne '2,2' -or $rgb -ne $expected[$index] -or $marker.anchor_from -ne $anchors[$index] -or $marker.anchor_to -ne $anchors[$index]) { throw 'Corner marker disagrees with the runtime geometry contract' }
}
$manifest = Get-Content -LiteralPath (Join-Path $pack 'manifest.json') -Raw | ConvertFrom-Json
if (($manifest.header.version -join '.') -ne '1.0.10' -or ($manifest.modules[0].version -join '.') -ne '1.0.10') { throw 'Resource pack version must be bumped for this layout' }
Write-Host 'Android home resource/native geometry contract: PASS'
