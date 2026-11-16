param([string]$Action='capture', [int]$X=-1, [int]$Y=-1, [string]$Output='build/ui-native-v2.png', [string]$Text='')
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class UiProbeNative {
 [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left,Top,Right,Bottom; }
 [StructLayout(LayoutKind.Sequential)] public struct Point { public int X,Y; }
 [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd,out Rect rect);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd,out Rect rect);
 [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr hwnd,int x,int y,int width,int height,bool repaint);
 [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hwnd,ref Point p);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hwnd,int command);
 [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hwnd);
 [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
 [DllImport("user32.dll")] public static extern void mouse_event(uint f,uint x,uint y,uint d,UIntPtr e);
 [DllImport("user32.dll")] public static extern void keybd_event(byte key,byte scan,uint flags,UIntPtr extra);
}
'@
$p=Get-Process -Name Minecraft.Windows -ErrorAction Stop | Select-Object -First 1
$h=$p.MainWindowHandle
if($h -eq 0){throw 'Client has no window'}
[UiProbeNative]::ShowWindow($h,9) | Out-Null
[UiProbeNative]::keybd_event(18,0,0,[UIntPtr]::Zero)
[UiProbeNative]::keybd_event(18,0,2,[UIntPtr]::Zero)
[UiProbeNative]::BringWindowToTop($h) | Out-Null
[UiProbeNative]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 350
if([UiProbeNative]::GetForegroundWindow() -ne $h){throw "Client did not receive foreground; refuse desktop screenshot/input"}
$r=New-Object UiProbeNative+Rect
$pt=New-Object UiProbeNative+Point
[UiProbeNative]::GetClientRect($h,[ref]$r) | Out-Null
[UiProbeNative]::ClientToScreen($h,[ref]$pt) | Out-Null
if($X -ge 0 -and $Y -ge 0){[UiProbeNative]::SetCursorPos($pt.X+$X,$pt.Y+$Y) | Out-Null}
if($Action -eq 'click'){
 Start-Sleep -Milliseconds 180
 [UiProbeNative]::mouse_event(2,0,0,0,[UIntPtr]::Zero)
 Start-Sleep -Milliseconds 80
 [UiProbeNative]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
}
if($Action -eq 'escape' -or $Action -eq 'enter'){
 [byte]$key=if($Action -eq 'escape'){27}else{13}
 [UiProbeNative]::keybd_event($key,0,0,[UIntPtr]::Zero)
 Start-Sleep -Milliseconds 100
 [UiProbeNative]::keybd_event($key,0,2,[UIntPtr]::Zero)
}
if($Action -eq 'paste'){
 $oldClipboard=Get-Clipboard -Raw -ErrorAction SilentlyContinue
 Set-Clipboard -Value $Text
 [UiProbeNative]::keybd_event(17,0,0,[UIntPtr]::Zero)
 [UiProbeNative]::keybd_event(86,0,0,[UIntPtr]::Zero)
 Start-Sleep -Milliseconds 180
 [UiProbeNative]::keybd_event(86,0,2,[UIntPtr]::Zero)
 [UiProbeNative]::keybd_event(17,0,2,[UIntPtr]::Zero)
 Start-Sleep -Milliseconds 150
 if($null -ne $oldClipboard){Set-Clipboard -Value $oldClipboard}
}
if($Action -eq 'small'){
 $bounds=New-Object UiProbeNative+Rect
 [UiProbeNative]::GetWindowRect($h,[ref]$bounds) | Out-Null
 $bounds | ConvertTo-Json | Set-Content build/client-window-bounds.json
 [UiProbeNative]::MoveWindow($h,80,80,1280,760,$true) | Out-Null
}
if($Action -eq 'restore'){
 $bounds=Get-Content build/client-window-bounds.json | ConvertFrom-Json
 [UiProbeNative]::MoveWindow($h,$bounds.Left,$bounds.Top,($bounds.Right-$bounds.Left),($bounds.Bottom-$bounds.Top),$true) | Out-Null
}
# Let the input get one rendered frame before capturing. This is not job polling.
Start-Sleep -Milliseconds 400
$b=New-Object System.Drawing.Bitmap($r.Right,$r.Bottom)
$g=[System.Drawing.Graphics]::FromImage($b)
$g.CopyFromScreen($pt.X,$pt.Y,0,0,$b.Size)
$b.Save((Join-Path (Get-Location) $Output),[System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose();$b.Dispose()
"client=$($r.Right)x$($r.Bottom) screenshot=$Output"
