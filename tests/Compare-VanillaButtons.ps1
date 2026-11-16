param(
 [string]$Image='build/ui-hover.png',
 [int]$X=836, [int]$Y=494, [int]$ReferenceY=558,
 [int]$Width=296, [int]$Height=60,
 [int]$TextX0=70, [int]$TextX1=226, [int]$TextY0=15, [int]$TextY1=47
)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
$b=[Drawing.Bitmap]::FromFile((Resolve-Path $Image))
try {
 $different=0; $total=0
 for($j=0;$j -lt $Height;$j++) {for($i=0;$i -lt $Width;$i++) {
  # Labels differ; compare only the actual skin, border and empty face pixels.
  if($i -ge $TextX0 -and $i -lt $TextX1 -and $j -ge $TextY0 -and $j -lt $TextY1){continue}
  $total++
  if($b.GetPixel($X+$i,$Y+$j).ToArgb() -ne $b.GetPixel($X+$i,$ReferenceY+$j).ToArgb()){$different++}
 }}
 "different=$different compared=$total image=$Image"
 if($different -ne 0){exit 1}
} finally {$b.Dispose()}
