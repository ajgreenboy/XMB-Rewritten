# Screen sampler for work/ingame-bg-test.ts. Read-only: it only copies screen pixels (no input, no process is touched).
# Every ~10 ms it writes "<epoch ms>,<foreground title>,<barX>,r,g,b,r,g,b,..." where r,g,b are single pixels at -Points
# (1920x1080 design coordinates, picked where the in-game XMB draws nothing) and barX is where the stand-in game's vertical
# yellow bar shows on a 1-pixel-high strip near the bottom of the monitor (-1 = no bar visible). The bar sweeps the screen at a
# known speed, so its position behind the XMB is an exact clock for WHICH game frame is on screen. A full-resolution PNG is
# saved every -ShotMs.
param([string]$Dir, [int]$Ms = 25000, [int]$ShotMs = 500, [string]$Points = '60,980;1860,980;60,120;1860,200;300,1010;1600,1010', [string]$Device = '')
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
Add-Type -ReferencedAssemblies System.Drawing @"
using System;using System.Drawing;using System.Drawing.Imaging;using System.Runtime.InteropServices;
public class SU{
 [DllImport("user32.dll")]public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern int GetWindowText(IntPtr h,System.Text.StringBuilder s,int n);
 // First x of a run of the dimmed bar colour (the in-game XMB leaves 20%: 255,217,26 -> 51,43,5). A dimmed field never reaches R 46.
 public static int FindBar(Bitmap bmp){
  var d=bmp.LockBits(new Rectangle(0,0,bmp.Width,1),ImageLockMode.ReadOnly,PixelFormat.Format32bppArgb);
  try{ int n=bmp.Width; var row=new byte[n*4]; Marshal.Copy(d.Scan0,row,0,n*4);
   for(int x=0;x<n;x++){ int b=row[x*4],g=row[x*4+1],r=row[x*4+2]; if(r>=46&&g>=36&&b<=14) return x; }
   return -1; } finally{ bmp.UnlockBits(d); }
 }
}
"@
$screen = if ($Device) { [System.Windows.Forms.Screen]::AllScreens | Where-Object { $_.DeviceName -like "*$Device" } | Select-Object -First 1 } else { [System.Windows.Forms.Screen]::PrimaryScreen }
$b = $screen.Bounds
New-Item -ItemType Directory -Force $Dir | Out-Null
$pts = $Points.Split(';') | ForEach-Object { $p = $_.Split(','); ,@([int]([double]$p[0] * $b.Width / 1920), [int]([double]$p[1] * $b.Height / 1080)) }
$stripY = [int](1040 * $b.Height / 1080)
$one = New-Object Drawing.Bitmap 1, 1
$g1 = [Drawing.Graphics]::FromImage($one)
$strip = New-Object Drawing.Bitmap $b.Width, 1
$gs = [Drawing.Graphics]::FromImage($strip)
$sw = [Diagnostics.Stopwatch]::StartNew()
$epoch0 = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
$lines = New-Object Collections.Generic.List[string]
$lastShot = -$ShotMs
$lastT = -100
$sb = New-Object Text.StringBuilder 100
while ($sw.ElapsedMilliseconds -lt $Ms) {
    if ($sw.ElapsedMilliseconds - $lastT -lt 10) { [Threading.Thread]::Sleep(1); continue }   # ~100 samples a second is plenty
    $lastT = $sw.ElapsedMilliseconds
    $t = $epoch0 + $sw.ElapsedMilliseconds
    [void]$sb.Clear(); [void][SU]::GetWindowText([SU]::GetForegroundWindow(), $sb, 100)
    $gs.CopyFromScreen($b.X, $b.Y + $stripY, 0, 0, [Drawing.Size]::new($b.Width, 1))
    $row = @("$t", $sb.ToString().Replace(',', ';'), "$([SU]::FindBar($strip))")
    foreach ($p in $pts) {
        $g1.CopyFromScreen($b.X + $p[0], $b.Y + $p[1], 0, 0, [Drawing.Size]::new(1, 1))
        $c = $one.GetPixel(0, 0)
        $row += "$($c.R)"; $row += "$($c.G)"; $row += "$($c.B)"
    }
    $lines.Add(($row -join ','))
    if ($sw.ElapsedMilliseconds - $lastShot -ge $ShotMs) {
        $lastShot = $sw.ElapsedMilliseconds
        $bmp = New-Object Drawing.Bitmap $b.Width, $b.Height
        $g = [Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($b.Location, [Drawing.Point]::Empty, $b.Size); $g.Dispose()
        $bmp.Save("$Dir\shot_$($epoch0 + $lastShot).png", [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    }
}
$g1.Dispose(); $one.Dispose(); $gs.Dispose(); $strip.Dispose()
$lines | Set-Content "$Dir\samples.csv"
"device=$($screen.DeviceName) bounds=$($b.X),$($b.Y) $($b.Width)x$($b.Height) stripY=$stripY samples=$($lines.Count)" | Set-Content "$Dir\meta.txt"
