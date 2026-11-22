# Gives the foreground to the top-level window of ONE process id (the XMB the test started), by the documented way through the
# foreground lock (attach to the current foreground thread's input queue for the call; the XMB does the same in View::bringXmbForward).
# Used by work/ingame-bg-test.ts so the XMB, which launches the stand-in, is the foreground application when the stand-in starts:
# a title launched by a background process is not allowed to take the foreground. Nothing is injected and no other window is touched.
param([int]$ProcessId)
Add-Type @"
using System;using System.Runtime.InteropServices;
public class FG{
 public delegate bool EnumProc(IntPtr h,IntPtr l);
 [DllImport("user32.dll")]public static extern bool EnumWindows(EnumProc p,IntPtr l);
 [DllImport("user32.dll")]public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
 [DllImport("user32.dll")]public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")]public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")]public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")]public static extern bool BringWindowToTop(IntPtr h);
 [DllImport("user32.dll")]public static extern bool AttachThreadInput(uint a,uint b,bool attach);
 [DllImport("kernel32.dll")]public static extern uint GetCurrentThreadId();
 public static IntPtr Find(uint pid){ IntPtr found=IntPtr.Zero; EnumWindows((h,l)=>{ uint p; GetWindowThreadProcessId(h,out p); if(p==pid&&IsWindowVisible(h)){found=h;return false;} return true;},IntPtr.Zero); return found; }
 public static bool Raise(IntPtr w){
  IntPtr fg=GetForegroundWindow(); if(fg==w) return true;
  uint dummy; uint ft=fg==IntPtr.Zero?0:GetWindowThreadProcessId(fg,out dummy); uint me=GetCurrentThreadId();
  bool att=ft!=0&&ft!=me&&AttachThreadInput(me,ft,true);
  SetForegroundWindow(w); BringWindowToTop(w);
  if(att) AttachThreadInput(me,ft,false);
  return GetForegroundWindow()==w;
 }
}
"@
$w = [FG]::Find([uint32]$ProcessId)
if ($w -eq [IntPtr]::Zero) { "no visible window for pid $ProcessId"; exit 1 }
"foreground taken: $([FG]::Raise($w))"
