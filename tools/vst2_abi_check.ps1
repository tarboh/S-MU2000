# Differential VST2 ABI parity gate: real SDK (local, licensed) vs clean-room compat.
#   tools/vst2_abi_check.ps1 [-Sdk D:\opt\vst\vstsdk2.4\pluginterfaces\vst2.x]
# Runs the arch-independent opcode/enum diff (python) plus a compiled
# sizeof/offsetof struct diff on BOTH architectures.
# Requires a licensed VST2 SDK on this machine; exits 1 on any parity break.
param(
  [string]$Sdk = "$env:VST2_SDK_DIR\pluginterfaces\vst2.x"
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path (Join-Path $Sdk 'aeffect.h'))) {
  throw "VST2 SDK not found at '$Sdk' (set VST2_SDK_DIR or pass -Sdk). The compat build itself never needs this; only this dev-box gate does."
}
$compat = Join-Path $repo 'cmake\vst2_compat'

Write-Host "== opcode/enum parity (arch-independent) =="
python (Join-Path $PSScriptRoot 'vst2_abi_check.py') --sdk $Sdk --compat $compat
if ($LASTEXITCODE -ne 0) { throw "opcode parity FAILED" }

$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvarsall = Join-Path $install 'VC\Auxiliary\Build\vcvarsall.bat'
if (-not (Test-Path $vcvarsall)) { throw "vcvarsall.bat not found" }
$tu = Join-Path $PSScriptRoot 'vst2_abi_check.cpp'

foreach ($arch in 'x64','Win32') {
  Write-Host "== struct layout parity ($arch) =="
  $toolset = if ($arch -eq 'x64') { 'amd64' } else { 'amd64_x86' }
  $outSdk = Join-Path $env:TEMP "vst2_abi_sdk_$arch.exe"
  $outCompat = Join-Path $env:TEMP "vst2_abi_compat_$arch.exe"
  $bat = Join-Path $env:TEMP "vst2_abi_build_$arch.bat"
  @"
call "$vcvarsall" $toolset >nul
cl /nologo /EHsc /std:c++17 /DSIDE_SDK /I"$Sdk" /Fo"$env:TEMP\abi_sdk_$arch.obj" /Fe"$outSdk" "$tu"
if errorlevel 1 exit /b 1
cl /nologo /EHsc /std:c++17 /I"$compat" /Fo"$env:TEMP\abi_compat_$arch.obj" /Fe"$outCompat" "$tu"
if errorlevel 1 exit /b 1
"@ | Set-Content -Path $bat -Encoding ascii
  & cmd /c "`"$bat`""
  if ($LASTEXITCODE -ne 0) { throw "struct-parity compile FAILED ($arch) — an SDK-only/compat-only member name is the finding" }
  $a = (& $outSdk) | ForEach-Object { $_ -replace '^sdk\s+','' }
  $b = (& $outCompat) | ForEach-Object { $_ -replace '^compat\s+','' }
  $diff = Compare-Object $a $b -SyncWindow 0
  if ($diff) {
    $diff | ForEach-Object { Write-Host ("{0} {1}" -f $_.SideIndicator, $_.InputObject) }
    throw "struct layout parity FAILED ($arch)"
  }
  Write-Host "struct layout parity OK ($arch, $($a.Count) entries)"
}
Write-Host "VST2 ABI PARITY GATE: PASS"
