# msvc32_build.ps1 — Win32 (x86) build harness for the native tools (verify/render/statetest).
# MSVC x86 (amd64_x86 cross env) because mingw32 cc1plus is silently broken on this box.
# Engine source list = Makefile OBJS (= engine/CMakeLists.txt target_sources minus the
# mu2000/vst3 entries); tool mains mirror the Makefile link lines.
#
#   pwsh -File tools/msvc32_build.ps1                      # all three tools
#   pwsh -File tools/msvc32_build.ps1 -Tools verify        # one tool
# Outputs: build32-msvc/<tool>.exe  (objs in build32-msvc/obj)

param(
  [string[]]$Tools = @('verify', 'render', 'statetest')
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$vcvars = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat'
if (-not (Test-Path -LiteralPath $vcvars)) { throw "vcvarsall.bat not found: $vcvars" }

$envDump = & cmd.exe /c "`"$vcvars`" amd64_x86 >nul 2>&1 && set"
if ($LASTEXITCODE -ne 0) { throw "vcvarsall amd64_x86 failed (exit $LASTEXITCODE)" }
foreach ($line in $envDump) {
  if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] }
}

$objDir = Join-Path $root 'build32-msvc\obj'
$outDir = Join-Path $root 'build32-msvc'
New-Item -ItemType Directory -Force -Path $objDir, $outDir | Out-Null

# Makefile OBJS (engine/CMakeLists.txt:13-30)
$engine = @(
  'src/compat/compat.cpp',
  'src/compat/a64asm.cpp',
  'src/smartmedia.cpp',
  'src/mame/sound/swp30.cpp',
  'src/mame/sound/swp30_jit.cpp',
  'src/mame/video/hd44780.cpp',
  'src/mame/machine/sci4.cpp',
  'src/mame/cpu/sh.cpp',
  'src/mame/cpu/sh2.cpp',
  'src/mame/cpu/sh2_jit.cpp',
  'src/mame/cpu/sh7042.cpp',
  'src/mame/cpu/sh_adc.cpp',
  'src/mame/cpu/sh_bsc.cpp',
  'src/mame/cpu/sh_cmt.cpp',
  'src/mame/cpu/sh_dmac.cpp',
  'src/mame/cpu/sh_intc.cpp',
  'src/mame/cpu/sh_mtu.cpp',
  'src/mame/cpu/sh_port.cpp',
  'src/mame/cpu/sh_sci.cpp'
)

# per-tool mains (Makefile link lines; verify does NOT link mu2000)
# midibench: the vst3 engine (SMU2000_VST2 MIDI-path A/B bench, tools/midi_bench.cpp) —
# same engine sources as the CMake smu2000_engine target, incl. its xgui stub.
$mainMap = @{
  verify    = @('src/verify.cpp')
  render    = @('src/mu2000.cpp', 'src/smf.cpp', 'src/render.cpp')
  statetest = @('src/mu2000.cpp', 'src/smf.cpp', 'src/statetest.cpp')
  midibench = @('src/mu2000.cpp', 'src/vst3/engine.cpp', 'engine/xgui_plugin_stub.cpp',
                'tools/midi_bench.cpp')
}

# accept both -Tools a,b (native arg passing arrives as one string) and -Tools a b
$Tools = @($Tools | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
foreach ($t in $Tools) {
  if (-not $mainMap.ContainsKey($t)) { throw "unknown tool: $t (verify|render|statetest)" }
}

$flags = @(
  '/nologo', '/std:c++20', '/O2', '/MT', '/utf-8', '/bigobj', '/EHsc', '/MP', '/c',
  '/D_USE_MATH_DEFINES', '/DNOMINMAX', '/D_CRT_SECURE_NO_WARNINGS',
  '/D_CRT_NONSTDC_NO_WARNINGS', '/DWIN32',
  '/Isrc', '/Isrc/compat', "/Fo$objDir\"
)

# unique sources across selected tools; recompile stale/missing only (flat obj names are unique)
$srcs = [System.Collections.Generic.SortedSet[string]]::new([System.StringComparer]::Ordinal)
foreach ($t in $Tools) { foreach ($s in ($engine + $mainMap[$t])) { [void]$srcs.Add($s) } }
$toCompile = @()
$allObjs = @()
foreach ($s in $srcs) {
  $obj = Join-Path $objDir ((Split-Path -Leaf $s) -replace '\.cpp$', '.obj')
  $allObjs += $obj
  $srcPath = Join-Path $root ($s -replace '/', '\')
  if (-not (Test-Path -LiteralPath $obj) -or
      (Get-Item -LiteralPath $srcPath).LastWriteTime -gt (Get-Item -LiteralPath $obj).LastWriteTime) {
    $toCompile += $s
  }
}

if ($toCompile.Count -gt 0) {
  Write-Host "== cl /c ($($toCompile.Count) files)"
  & cl.exe @flags @toCompile
  if ($LASTEXITCODE -ne 0) { throw "cl failed (exit $LASTEXITCODE)" }
} else {
  Write-Host "== cl /c (all objs up to date)"
}

foreach ($t in $Tools) {
  $toolObjs = @()
  foreach ($s in ($engine + $mainMap[$t])) {
    $toolObjs += Join-Path $objDir ((Split-Path -Leaf $s) -replace '\.cpp$', '.obj')
  }
  $exe = Join-Path $outDir "$t.exe"
  Write-Host "== link $t.exe"
  & link.exe /nologo "/OUT:$exe" @toolObjs
  if ($LASTEXITCODE -ne 0) { throw "link $t failed (exit $LASTEXITCODE)" }
  $machine = (& dumpbin.exe /HEADERS $exe | Select-String -Pattern 'machine \(' | Select-Object -First 1).Line.Trim()
  Write-Host "== $exe  [$machine]"
  if ($machine -notmatch '14C') { throw "$t.exe is not x86: $machine" }
}
Write-Host "== done: $($Tools -join ', ')"
