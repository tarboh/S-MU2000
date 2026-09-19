# Builds + runs the native VST2 host probe against a given plugin DLL.
#   tools/vst2_probe_run.ps1 -Arch x64   [-Dll <path>] [-PumpMs 2500]
# Defaults: the clean-room-ABI Release build under build-cmake/vst2/<Arch>/Release.
param(
  [ValidateSet('x64','Win32')][string]$Arch = 'x64',
  [string]$Dll = "",
  [int]$PumpMs = 2500
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $Dll) { $Dll = Join-Path $repo "build-cmake\vst2\$Arch\Release\SMU2000_VST2.dll" }
if (-not (Test-Path $Dll)) { throw "plugin dll not found: $Dll" }
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$install = ''
if (Test-Path $vswhere) {
  $install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
$vcvarsall = if ($install) { Join-Path $install 'VC\Auxiliary\Build\vcvarsall.bat' } else { '' }
if (-not (Test-Path $vcvarsall)) { throw "vcvarsall.bat not found (install='$install')" }
$out = Join-Path $env:TEMP "vst2_host_probe_$Arch.exe"
$toolsetArg = if ($Arch -eq 'x64') { 'amd64' } else { 'amd64_x86' }
$bat = @"
call "$vcvarsall" $toolsetArg >nul
cl /nologo /EHsc /std:c++17 /I"$repo\iPlug2\Dependencies\IPlug\VST2_SDK" /Fo"$env:TEMP\probe_$Arch.obj" /Fe"$out" "$repo\tools\vst2_host_probe.cpp" /link user32.lib gdi32.lib
"@
$batFile = Join-Path $env:TEMP "vst2_probe_build_$Arch.bat"
Set-Content -Path $batFile -Value $bat -Encoding ascii
& cmd /c "`"$batFile`""
if (-not (Test-Path $out)) { throw "probe build failed" }
Write-Host "probe built: $out"
& $out $Dll $PumpMs
Write-Host "probe exit: $LASTEXITCODE"
exit $LASTEXITCODE
