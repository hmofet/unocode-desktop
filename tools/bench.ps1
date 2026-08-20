# UnoCode Desktop vs Visual Studio Code: black-box benchmark, instrumented.
#
# Neither application is modified or instrumented internally, so neither gets to
# define its own finish line. Both are watched the same way: sample the SCREEN
# where the window is, every ~8 ms, counting distinct colours in a fixed grid.
# That trace yields three honest milestones:
#
#   tWindow   a window exists on screen
#   tContent  it has painted real content (>= $CONTENT distinct colours)
#   tSettled  the picture has stopped changing (stable for $STABLE_MS)
#
# tSettled is the one to quote: it is threshold-insensitive and it is the moment
# a user would say "it's up".

param(
    [int]$Runs = 5,
    [string]$Workspace,
    [string]$Out = "bench.json",
    [string]$UnoExe = "C:\Repos\unocode-desktop\build-local\win\unocode.exe",
    [string]$CodeExe = "$env:LOCALAPPDATA\Programs\Microsoft VS Code\Code.exe",
    [string]$BigFile = "",
    [string]$ShotDir = ""
)

Add-Type -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public class Shot {
    [DllImport("user32.dll")] static extern IntPtr GetDesktopWindow();
    [DllImport("user32.dll")] static extern IntPtr GetWindowDC(IntPtr h);
    [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr h, IntPtr dc);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("gdi32.dll")] static extern IntPtr CreateCompatibleDC(IntPtr dc);
    [DllImport("gdi32.dll")] static extern IntPtr CreateCompatibleBitmap(IntPtr dc, int w, int h);
    [DllImport("gdi32.dll")] static extern IntPtr SelectObject(IntPtr dc, IntPtr o);
    [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr o);
    [DllImport("gdi32.dll")] static extern bool DeleteDC(IntPtr dc);
    [DllImport("gdi32.dll")] static extern bool BitBlt(IntPtr d,int dx,int dy,int w,int h,IntPtr s,int sx,int sy,int rop);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }

    static Bitmap Grab(IntPtr hwnd, out int w, out int h) {
        w = 0; h = 0; RECT r;
        if (!IsWindowVisible(hwnd) || !GetWindowRect(hwnd, out r)) return null;
        w = r.R - r.L; h = r.B - r.T;
        if (w < 50 || h < 50) return null;
        IntPtr screen = GetWindowDC(GetDesktopWindow());
        IntPtr mem = CreateCompatibleDC(screen);
        IntPtr bmp = CreateCompatibleBitmap(screen, w, h);
        IntPtr old = SelectObject(mem, bmp);
        BitBlt(mem, 0, 0, w, h, screen, r.L, r.T, 0x00CC0020);
        SelectObject(mem, old);
        Bitmap b = Bitmap.FromHbitmap(bmp);
        DeleteObject(bmp); DeleteDC(mem); ReleaseDC(GetDesktopWindow(), screen);
        return b;
    }

    public static int Signature(IntPtr hwnd) {
        int w, h; Bitmap b = Grab(hwnd, out w, out h);
        if (b == null) return -1;
        HashSet<int> seen = new HashSet<int>();
        int sx = Math.Max(1, w / 60), sy = Math.Max(1, h / 40);
        for (int y = 0; y < h; y += sy)
            for (int x = 0; x < w; x += sx)
                seen.Add(b.GetPixel(x, y).ToArgb());
        b.Dispose();
        return seen.Count;
    }

    public static void Save(IntPtr hwnd, string path) {
        int w, h; Bitmap b = Grab(hwnd, out w, out h);
        if (b == null) return;
        b.Save(path, ImageFormat.Png); b.Dispose();
    }
    public static int[] Size(IntPtr hwnd) {
        RECT r; GetWindowRect(hwnd, out r);
        return new int[] { r.R - r.L, r.B - r.T };
    }
}
'@ -ReferencedAssemblies System.Drawing, System.Collections

$CONTENT   = 14      # distinct colours meaning "real content is on screen"
$STABLE_MS = 500     # unchanged for this long = finished painting

function Get-Tree($rootPid) {
    $all = Get-CimInstance Win32_Process | Select-Object ProcessId, ParentProcessId
    $set = @{}; $set[[int]$rootPid] = $true
    for ($i = 0; $i -lt 8; $i++) {
        foreach ($p in $all) { if ($set[[int]$p.ParentProcessId]) { $set[[int]$p.ProcessId] = $true } }
    }
    $set.Keys
}

function Measure-Launch($exe, $argList, $label, $shot) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $proc = Start-Process -FilePath $exe -ArgumentList $argList -PassThru
    $tWindow = -1; $tContent = -1; $tSettled = -1
    $hwnd = [IntPtr]::Zero
    $trace = New-Object System.Collections.ArrayList
    $lastSig = -999; $stableSince = -1; $winSize = @(0,0)

    while ($sw.Elapsed.TotalSeconds -lt 90) {
        if ($hwnd -eq [IntPtr]::Zero) {
            foreach ($p in (Get-Tree $proc.Id)) {
                $q = Get-Process -Id $p -ErrorAction SilentlyContinue
                if ($q -and $q.MainWindowHandle -ne 0) { $hwnd = $q.MainWindowHandle; break }
            }
            if ($hwnd -ne [IntPtr]::Zero) { $tWindow = $sw.Elapsed.TotalMilliseconds }
        }
        if ($hwnd -ne [IntPtr]::Zero) {
            $t = $sw.Elapsed.TotalMilliseconds
            $sig = [Shot]::Signature($hwnd)
            [void]$trace.Add(@($([int]$t), $sig))
            if ($sig -ge $CONTENT -and $tContent -lt 0) {
                $tContent = $t
                if ($shot) { [Shot]::Save($hwnd, $shot) }
                $winSize = [Shot]::Size($hwnd)
            }
            if ($sig -ge $CONTENT -and [math]::Abs($sig - $lastSig) -le 1) {
                if ($stableSince -lt 0) { $stableSince = $t }
                elseif (($t - $stableSince) -ge $STABLE_MS) { $tSettled = $stableSince; break }
            } else { $stableSince = -1 }
            $lastSig = $sig
        }
        Start-Sleep -Milliseconds 8
    }

    Start-Sleep -Seconds 5
    $pids = Get-Tree $proc.Id
    $mem = 0; $priv = 0; $nproc = 0
    foreach ($p in $pids) {
        $q = Get-Process -Id $p -ErrorAction SilentlyContinue
        if ($q) { $mem += $q.WorkingSet64; $priv += $q.PrivateMemorySize64; $nproc++ }
    }
    # TRUE idle: give it time to stop working before asking what it costs at rest
    Start-Sleep -Seconds 10
    $c0 = 0.0; foreach ($p in $pids) { $q = Get-Process -Id $p -ErrorAction SilentlyContinue; if ($q) { $c0 += $q.TotalProcessorTime.TotalMilliseconds } }
    Start-Sleep -Seconds 8
    $c1 = 0.0; foreach ($p in $pids) { $q = Get-Process -Id $p -ErrorAction SilentlyContinue; if ($q) { $c1 += $q.TotalProcessorTime.TotalMilliseconds } }
    $cores = [Environment]::ProcessorCount
    $idleCpu = [math]::Round(($c1 - $c0) / 8000.0 * 100.0, 2)

    foreach ($p in $pids) { Stop-Process -Id $p -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Seconds 2

    [pscustomobject]@{
        label = $label
        tWindowMs = [int]$tWindow; tContentMs = [int]$tContent; tSettledMs = [int]$tSettled
        memMB = [math]::Round($mem / 1MB, 1); privMB = [math]::Round($priv / 1MB, 1); procs = $nproc
        idleCpuPct = $idleCpu; cores = $cores
        winW = $winSize[0]; winH = $winSize[1]
        trace = $trace
    }
}

$freshArgs = @("--user-data-dir", "$env:TEMP\bench-vscode-ud",
               "--extensions-dir", "$env:TEMP\bench-vscode-ext",
               "--disable-workspace-trust", "--new-window", $Workspace)
# what the user actually launches: their real profile and extensions
$realArgs  = @("--new-window", $Workspace)

$results = @()
Write-Host "warmup (discarded)..."
$null = Measure-Launch $UnoExe @($Workspace) "warmup" $null
$null = Measure-Launch $CodeExe $freshArgs "warmup" $null
$null = Measure-Launch $CodeExe $realArgs  "warmup" $null

for ($i = 1; $i -le $Runs; $i++) {
    Write-Host "run $i/$Runs"
    $s1 = if ($i -eq 1 -and $ShotDir) { "$ShotDir\open-unocode.png" } else { $null }
    $s2 = if ($i -eq 1 -and $ShotDir) { "$ShotDir\open-vscode.png" } else { $null }
    $results += Measure-Launch $UnoExe  @($Workspace) "unocode"       $s1
    $results += Measure-Launch $CodeExe $freshArgs    "vscode-clean"  $s2
    $results += Measure-Launch $CodeExe $realArgs     "vscode-real"   $null
}

if ($BigFile) {
    Write-Host "big file (4 MB / 60k lines)..."
    for ($i = 1; $i -le 3; $i++) {
        $s3 = if ($i -eq 1 -and $ShotDir) { "$ShotDir\big-unocode.png" } else { $null }
        $s4 = if ($i -eq 1 -and $ShotDir) { "$ShotDir\big-vscode.png" } else { $null }
        $results += Measure-Launch $UnoExe  @($Workspace, "--open", (Split-Path -Leaf $BigFile)) "unocode-big" $s3
        $results += Measure-Launch $CodeExe ($freshArgs + @($BigFile)) "vscode-clean-big" $s4
    }
}

$results | ConvertTo-Json -Depth 6 -Compress | Set-Content -Encoding utf8 $Out

function Med($xs) { $s = @($xs | Sort-Object); $s[[int](($s.Count - 1) / 2)] }
$results | Group-Object label | ForEach-Object {
    $g = $_.Group
    [pscustomobject]@{
        app = $_.Name; n = $g.Count
        settledMs = [int](Med $g.tSettledMs)
        contentMs = [int](Med $g.tContentMs)
        windowMs  = [int](Med $g.tWindowMs)
        memMB     = [double](Med $g.memMB)
        privMB    = [double](Med $g.privMB)
        procs     = [int](Med $g.procs)
        idleCpu   = [double](Med $g.idleCpuPct)
    }
} | Format-Table -AutoSize
