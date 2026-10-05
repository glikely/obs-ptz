<#
.SYNOPSIS
  Saves a PNG of one window on the interactive Windows desktop.

.DESCRIPTION
  Used by scripts/update-screenshots.py. Finds the visible top level window
  whose title is -Title (or starts with it, with -Prefix), optionally moves
  and resizes it first, brings it to the front, and saves what is on the
  screen where it is, without its invisible resize border. It has to run in
  the logged in user's session: from a service or SYSTEM there is no desktop
  to copy.

.EXAMPLE
  windows-capture.ps1 -Title "PTZ Controls" -Out dock.png
  windows-capture.ps1 -Title "OBS " -Prefix -Out obs.png -X 0 -Y 0 -W 1400 -H 900
#>
param(
	[Parameter(Mandatory)][string]$Title,
	[Parameter(Mandatory)][string]$Out,
	[switch]$Prefix,
	[Nullable[int]]$X, [Nullable[int]]$Y, [Nullable[int]]$W, [Nullable[int]]$H
)

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class Win {
	public delegate bool EnumProc(IntPtr h, IntPtr l);
	[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
	[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
	[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
	[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
	[DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int hgt, bool repaint);
	[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
	[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
	[DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out RECT r, int size);
	[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }

	public static IntPtr Find(string title, bool prefix) {
		IntPtr found = IntPtr.Zero;
		EnumWindows((h, l) => {
			if (!IsWindowVisible(h)) return true;
			var sb = new StringBuilder(256);
			GetWindowText(h, sb, 256);
			string t = sb.ToString();
			if (prefix ? t.StartsWith(title) : t == title) { found = h; return false; }
			return true;
		}, IntPtr.Zero);
		return found;
	}
	/* The window as drawn: GetWindowRect includes the invisible resize border */
	public static RECT Frame(IntPtr h) {
		RECT r;
		DwmGetWindowAttribute(h, 9 /* DWMWA_EXTENDED_FRAME_BOUNDS */, out r, Marshal.SizeOf(typeof(RECT)));
		return r;
	}
}
'@

[void][Win]::SetProcessDPIAware()
$h = [Win]::Find($Title, $Prefix.IsPresent)
if ($h -eq [IntPtr]::Zero) { Write-Error "no window titled '$Title'"; exit 1 }

[void][Win]::ShowWindow($h, 9)   # SW_RESTORE
if ($null -ne $W -and $null -ne $H) {
	[void][Win]::MoveWindow($h, [int]$X, [int]$Y, $W, $H, $true)
}
[void][Win]::SetForegroundWindow($h)
Start-Sleep -Milliseconds 800

$r = [Win]::Frame($h)
$width = $r.Right - $r.Left
$height = $r.Bottom - $r.Top
$bmp = New-Object System.Drawing.Bitmap $width, $height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size $width, $height))
$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output "$Out ${width}x${height}"
