# Builds DXUP-Setup-<version>.exe: DXUP's DLLs from build_rel (x64) and
# build_rel32 (x86, for 32-bit games), built with ninja beforehand, or here
# with -Native from a Visual Studio developer shell, DXUP Manager and the
# setup (.NET Framework 4.8.1, dotnet SDK). -Upscalers <folder> adds the
# upscalers' runtime DLLs and licenses from the SDK folder (third_party:
# dlss, ffx, xess) as bin\upscalers, where DXUP's d3d11.dll finds them, and
# names the setup DXUP-Setup-<version>-Upscalers.exe (a release has both).
param([switch]$Native, [string]$Upscalers = '')
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$version = (Select-String -Path (Join-Path $root 'meson.build') -Pattern "version\s*:\s*'([0-9.]+)'").Matches[0].Groups[1].Value

if ($Native) { & ninja -C (Join-Path $root 'build_rel'); if ($LASTEXITCODE) { throw 'ninja failed' } }
# Direct3D 10/11 and DXGI, and the legacy APIs (deployed to games that use them).
$sources = @{ 'd3d11.dll' = 'd3d11'; 'dxgi.dll' = 'd3d11'; 'ddraw.dll' = 'ddraw'; 'd3d8.dll' = 'd3d8'; 'd3d9.dll' = 'd3d9' }
foreach ($dll in $sources.Keys) {
  foreach ($build in 'build_rel', 'build_rel32') {
    if (-not (Test-Path (Join-Path $root "$build\src\$($sources[$dll])\$dll"))) { throw "$dll missing: build $build first" }
  }
}

$package = Join-Path $root 'artifacts\package'
$payload = Join-Path $package 'payload'
if (Test-Path $payload) { Remove-Item -Recurse -Force $payload }
New-Item -ItemType Directory -Force -Path (Join-Path $payload 'bin'), (Join-Path $payload 'bin32') | Out-Null

& dotnet build (Join-Path $root 'manager\DxupManager\DxupManager.csproj') -c Release -nologo -v q "-p:Version=$version"
if ($LASTEXITCODE) { throw 'manager build failed' }
$managerOut = Join-Path $root 'manager\DxupManager\bin\Release\net481'
Copy-Item (Join-Path $managerOut 'DXUP Manager.exe'), (Join-Path $managerOut 'DXUP Manager.exe.config') $payload
foreach ($dll in $sources.Keys) {
  Copy-Item (Join-Path $root "build_rel\src\$($sources[$dll])\$dll") (Join-Path $payload 'bin')
  Copy-Item (Join-Path $root "build_rel32\src\$($sources[$dll])\$dll") (Join-Path $payload 'bin32')
}
Copy-Item (Join-Path $root 'manager\README.txt') $payload
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $payload 'LICENSE.txt')
if ($Upscalers) {
  $target = Join-Path $payload 'bin\upscalers'
  New-Item -ItemType Directory -Force -Path (Join-Path $target 'licenses') | Out-Null
  foreach ($file in 'dlss\bin\nvngx_dlss.dll', 'ffx\bin\amd_fidelityfx_loader_dx12.dll',
                    'ffx\bin\amd_fidelityfx_upscaler_dx12.dll', 'xess\bin\libxess.dll') {
    Copy-Item (Join-Path $Upscalers $file) $target
  }
  Copy-Item (Join-Path $Upscalers 'dlss\LICENSE.txt') (Join-Path $target 'licenses\NVIDIA-DLSS-LICENSE.txt')
  Copy-Item (Join-Path $Upscalers 'ffx\license.md') (Join-Path $target 'licenses\AMD-FidelityFX-LICENSE.md')
  Copy-Item (Join-Path $Upscalers 'xess\LICENSE.txt') (Join-Path $target 'licenses\Intel-XeSS-LICENSE.txt')
}

& dotnet build (Join-Path $root 'manager\DxupSetup\DxupSetup.csproj') -c Release -nologo -v q "-p:Version=$version" "-p:PayloadDir=$payload" --no-incremental
if ($LASTEXITCODE) { throw 'setup build failed' }
$setup = Join-Path $package ("DXUP-Setup-$version" + $(if ($Upscalers) { '-Upscalers' } else { '' }) + '.exe')
Copy-Item (Join-Path $root 'manager\DxupSetup\bin\Release\net481\DXUP-Setup.exe') $setup -Force
Write-Output "built $setup"
