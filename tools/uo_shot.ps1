# Brings the game window to the front and saves a downscaled screenshot of it to $Out.
param([string]$Out)
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class W {
  [StructLayout(LayoutKind.Sequential)] public struct R { public int L, T, Ri, B; }
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref System.Drawing.Point p);
}
'@ -ReferencedAssemblies System.Drawing
[void][W]::SetProcessDPIAware()
$p = Get-Process vwport | Select-Object -First 1
$h = $p.MainWindowHandle
[void][W]::ShowWindow($h, 9); [void][W]::SetForegroundWindow($h); Start-Sleep -Milliseconds 400
$r = New-Object W+R; [void][W]::GetClientRect($h, [ref]$r)
$pt = New-Object System.Drawing.Point 0, 0; [void][W]::ClientToScreen($h, [ref]$pt)
$w = $r.Ri; $hh = $r.B
$bmp = New-Object System.Drawing.Bitmap $w, $hh
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($pt, [System.Drawing.Point]::Empty, (New-Object System.Drawing.Size $w, $hh))
$sw = 1280; $sh = [int]($hh * $sw / $w)
(New-Object System.Drawing.Bitmap $bmp, $sw, $sh).Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
"${w}x${hh} at $($pt.X),$($pt.Y)"
