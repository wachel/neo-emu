# KOF98 Native -- Windows one-click build (PowerShell).
# Builds: build\kof98native.exe (playable) + build\lib\kof98.dll (RL interface).
# Prerequisite: zig 0.14.0 -- either extracted under tools\ or on PATH.
#   https://ziglang.org/download/  (windows-x86_64)
$ErrorActionPreference = "Stop"
Set-Location (Split-Path -Parent $MyInvocation.MyCommand.Path)

# ---- locate zig ----
$zig = $null
$cand = Get-ChildItem -Path "tools" -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like "zig*" } |
        ForEach-Object { Join-Path $_.FullName "zig.exe" } |
        Where-Object { Test-Path $_ } | Select-Object -First 1
if ($cand) { $zig = $cand }
elseif (Get-Command zig -ErrorAction SilentlyContinue) { $zig = "zig" }
else {
    Write-Error @"
zig not found. Either:
  1. download zig 0.14.0 (windows-x86_64) and extract to tools\zig-windows-x86_64-0.14.0\, or
  2. put zig.exe on PATH.
"@
}

Write-Output "using zig: $zig"
New-Item -ItemType Directory -Path build, build\lib -Force | Out-Null

# x86_64_v3 = AVX2 baseline (2013+ CPUs). Use x86_64_v2 for older machines.
$CPU = if ($env:KOF98_CPU) { $env:KOF98_CPU } else { "x86_64_v3" }
$OPT = "-O3", "-std=c++17", "-mcpu=$CPU", "-Isrc"

function Compile($src, $obj) {
    & $zig c++ @OPT -c -o $obj $src
    if ($LASTEXITCODE -ne 0) { Write-Error "compile failed: $src" }
}

$core  = "emu","video","cpu_interp","cpu_interp2","z80","ym2610","romload"
$ymfm  = "ymfm_adpcm","ymfm_opn","ymfm_ssg"

foreach ($s in $core) { Compile "src\$s.cpp"  "build\lib\$s.o" }
foreach ($s in $ymfm) { Compile "src\ymfm\$s.cpp" "build\lib\$s.o" }
Compile "src\main.cpp" "build\lib\main.o"
Compile "src\kof98_api.cpp" "build\lib\kof98_api.o"

$objs = $core + $ymfm | ForEach-Object { "build\lib\$_.o" }

$mcpu = "-mcpu=$CPU"

# playable exe (Win32 window + waveOut audio)
& $zig c++ -O3 $mcpu -o build\kof98native.exe ($objs + "build\lib\main.o") -lwinmm -lgdi32 -luser32
if ($LASTEXITCODE -ne 0) { Write-Error "link exe failed" }

# RL interface dll (no window/audio deps)
& $zig c++ -shared -O3 $mcpu -o build\lib\kof98.dll ($objs + "build\lib\kof98_api.o")
if ($LASTEXITCODE -ne 0) { Write-Error "link dll failed" }

Write-Output ""
Write-Output "OK: build\kof98native.exe (playable)"
Write-Output "OK: build\lib\kof98.dll     (RL interface)"
Write-Output "put roms\kof98.zip + roms\neogeo.zip next to the exe (or pass roms dir to the API)"
