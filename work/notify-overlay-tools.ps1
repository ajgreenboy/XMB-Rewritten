# Helpers for work/notify-overlay-e2e.ts (audit/INFO-BAR.md, in-game toast).
#   -Mode shot    -Out <png>            capture the primary monitor and write <png>.json (monitor size, foreground window,
#                                        and every visible topmost tool window: class, rect, extended styles)
#   -Mode backdrop -Seconds <n>         show a borderless full-screen game-like gradient for n seconds (a neutral stand-in for a
#                                        running game's picture, so the capture shows the overlay over a game, not the XMB behind it)
#   -Mode compose -Xmb <png> -Game <png> -Out <png>
#                                        crop the toast's region (design 1100..1920 x 60..210) from an XMB grab and from a
#                                        monitor capture, stack them with a label, scale x2, write <png>
param([string]$Mode, [string]$Out, [string]$Xmb, [string]$Game, [string]$Ref, [int]$Seconds = 8)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Text; using System.Runtime.InteropServices; using System.Collections.Generic;
public static class W {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int i);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  public static List<string> Topmost() {
    var o = new List<string>();
    EnumWindows((h, l) => {
      if (!IsWindowVisible(h)) return true;
      int ex = GetWindowLong(h, -20);
      if ((ex & 0x8) == 0) return true;   // WS_EX_TOPMOST only
      RECT r; GetWindowRect(h, out r);
      var c = new StringBuilder(256); GetClassName(h, c, 256);
      var t = new StringBuilder(256); GetWindowText(h, t, 256);
      o.Add(string.Format("{0}|{1}|{2},{3},{4},{5}|{6}", h.ToInt64(), c, r.L, r.T, r.R - r.L, r.B - r.T, ex) + "|" + t);
      return true;
    }, IntPtr.Zero);
    return o;
  }
}
'@
[void][W]::SetProcessDPIAware()
if ($Mode -eq 'backdrop') {
  Add-Type -AssemblyName System.Windows.Forms
  $f = New-Object System.Windows.Forms.Form
  $f.FormBorderStyle = 'None'; $f.StartPosition = 'Manual'; $f.Bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds; $f.Text = 'Notify Overlay Backdrop'
  $f.add_Paint({ param($s, $e)
    $r = $s.ClientRectangle
    $b = New-Object System.Drawing.Drawing2D.LinearGradientBrush $r, ([System.Drawing.Color]::FromArgb(40, 70, 120)), ([System.Drawing.Color]::FromArgb(150, 110, 60)), 35.0
    $e.Graphics.FillRectangle($b, $r)
    $p = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(90, 255, 255, 255)), 3
    for ($i = 0; $i -lt 12; $i++) { $e.Graphics.DrawLine($p, 0, $i * 100, $r.Width, $i * 100 - 300) }
  })
  $t = New-Object System.Windows.Forms.Timer; $t.Interval = $Seconds * 1000; $t.add_Tick({ $f.Close() })
  $f.add_Shown({ $t.Start() })
  [void]$f.ShowDialog()
}
elseif ($Mode -eq 'shot') {
  $w = [W]::GetSystemMetrics(0); $h = [W]::GetSystemMetrics(1)
  $bmp = New-Object System.Drawing.Bitmap $w, $h
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen(0, 0, 0, 0, $bmp.Size)
  $g.Dispose(); $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
  $fg = [W]::GetForegroundWindow().ToInt64()
  $info = [ordered]@{ width = $w; height = $h; foreground = $fg; topmost = @([W]::Topmost()) }
  $info | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 "$Out.json"
}
elseif ($Mode -eq 'compose') {
  $crop = {
    param($path)
    $src = New-Object System.Drawing.Bitmap $path
    $s = $src.Width / 1920.0
    $rect = New-Object System.Drawing.Rectangle ([int](1100 * $s)), ([int](60 * $s)), ([int](820 * $s)), ([int](150 * $s))
    $dst = New-Object System.Drawing.Bitmap 820, 150
    $g = [System.Drawing.Graphics]::FromImage($dst)
    $g.InterpolationMode = 'HighQualityBicubic'
    $g.DrawImage($src, (New-Object System.Drawing.Rectangle 0, 0, 820, 150), $rect, 'Pixel')
    $g.Dispose(); $src.Dispose(); $dst
  }
  $a = & $crop $Xmb; $b = & $crop $Game
  $label = 22
  $rows = @(@('XMB toast (View::drawNotification)', $a), @('In-game NotifyOverlay window over a stand-in title', $b))
  if ($Ref) { $rows += ,@('Recording M2, 51 s (720p frame, same crop)', (& $crop $Ref)) }
  $sheet = New-Object System.Drawing.Bitmap 1640, (($label + 150) * 2 * $rows.Count)
  $g = [System.Drawing.Graphics]::FromImage($sheet)
  $g.Clear([System.Drawing.Color]::FromArgb(24, 24, 24))
  $g.InterpolationMode = 'NearestNeighbor'
  $font = New-Object System.Drawing.Font 'Segoe UI', 16
  $y = 0
  foreach ($pair in $rows) {
    $g.DrawString($pair[0], $font, [System.Drawing.Brushes]::White, 6, $y + 4)
    $g.DrawImage($pair[1], (New-Object System.Drawing.Rectangle 0, ($y + $label * 2), 1640, 300))
    $y += ($label + 150) * 2
  }
  $g.Dispose(); $sheet.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $sheet.Dispose(); $a.Dispose(); $b.Dispose()
}
