# Desktop composite probe for the in-game overlay test (work/overlay-live-test.ts). Captures the primary screen twice,
# 400 ms apart, and prints one JSON line: mean luma of the whole screen, and how many pixels of the corner regions the XMB
# columns never draw on (left 25% and bottom 15%) changed between the two captures. A running game behind a live overlay
# keeps changing there; a frozen capture does not.
param([int]$Gap = 400)
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
Add-Type -ReferencedAssemblies System.Drawing, System.Windows.Forms -TypeDefinition @'
using System; using System.Drawing; using System.Drawing.Imaging;
public static class Probe {
  public static byte[] Grab(out int w, out int h) {
    var s = System.Windows.Forms.Screen.PrimaryScreen.Bounds; w = s.Width; h = s.Height;
    using (var b = new Bitmap(w, h, PixelFormat.Format32bppArgb))
    using (var g = Graphics.FromImage(b)) {
      g.CopyFromScreen(s.X, s.Y, 0, 0, b.Size);
      var d = b.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
      var bytes = new byte[d.Stride * h]; System.Runtime.InteropServices.Marshal.Copy(d.Scan0, bytes, 0, bytes.Length);
      b.UnlockBits(d); return bytes;
    }
  }
  public static string Compare(byte[] a, byte[] b, int w, int h) {
    int stride = w * 4; long changed = 0, region = 0; double luma = 0; long lumaN = 0;
    for (int y = 0; y < h; y += 2) for (int x = 0; x < w; x += 2) {
      int i = y * stride + x * 4;
      luma += 0.114 * b[i] + 0.587 * b[i + 1] + 0.299 * b[i + 2]; lumaN++;
      if (x < w / 4 || y > h * 0.85) {
        region++;
        if (Math.Abs(a[i] - b[i]) + Math.Abs(a[i + 1] - b[i + 1]) + Math.Abs(a[i + 2] - b[i + 2]) > 6) changed++;
      }
    }
    return "{\"width\":" + w + ",\"height\":" + h + ",\"meanLuma\":" + (luma / lumaN).ToString("F2", System.Globalization.CultureInfo.InvariantCulture) + ",\"regionPixels\":" + region + ",\"changed\":" + changed + "}";
  }
}
'@
$w = 0; $h = 0
$a = [Probe]::Grab([ref]$w, [ref]$h)
Start-Sleep -Milliseconds $Gap
$b = [Probe]::Grab([ref]$w, [ref]$h)
[Probe]::Compare($a, $b, $w, $h)
