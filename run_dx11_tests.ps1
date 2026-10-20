param(
  [switch]$Build,
  [switch]$Stress,
  [switch]$StressOnly,
  [int]$StressIterations = 0,
  [string]$BuildDir = 'build_d3d12_x64',
  [string]$ArtifactDir = 'artifacts\test_runs'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($Stress -and $StressOnly) {
  throw 'Use either -Stress or -StressOnly, not both.'
}

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildRoot = Join-Path $repoRoot $BuildDir
$testBuildDir = Join-Path $buildRoot 'src\tests'
$wrapperBuildDir = Join-Path $buildRoot 'src\d3d11'

$sourceExe = Join-Path $testBuildDir 'dx11_test_app.exe'
$sourceWrapperD3D11 = Join-Path $wrapperBuildDir 'd3d11.dll'
$sourceWrapperDxgi = Join-Path $wrapperBuildDir 'dxgi.dll'

$runRoot = Join-Path $repoRoot $ArtifactDir
$logDir = Join-Path $runRoot 'logs'
$systemRunDir = Join-Path $runRoot 'system'
$wrapperRunDir = Join-Path $runRoot 'wrapper'

function Invoke-Build {
  Write-Host '==> Building DXUP'
  Push-Location $repoRoot
  try {
    & ninja -C $BuildDir
    if ($LASTEXITCODE -ne 0) {
      throw "Build failed with exit code $LASTEXITCODE."
    }
  }
  finally {
    Pop-Location
  }
}

function Reset-RunDirectory {
  param(
    [string]$Path
  )

  if (Test-Path $Path) {
    Remove-Item $Path -Recurse -Force
  }

  New-Item -ItemType Directory -Path $Path | Out-Null
}

function Stage-SystemRun {
  Reset-RunDirectory -Path $systemRunDir
  Copy-Item $sourceExe $systemRunDir
}

function Stage-WrapperRun {
  Reset-RunDirectory -Path $wrapperRunDir
  Copy-Item $sourceExe $wrapperRunDir
  Copy-Item $sourceWrapperD3D11 $wrapperRunDir
  Copy-Item $sourceWrapperDxgi $wrapperRunDir
}

function Invoke-TestRun {
  param(
    [string]$Label,
    [string]$WorkingDirectory,
    [string]$LogName,
    [string[]]$Arguments
  )

  $logPath = Join-Path $logDir $LogName

  Write-Host "==> Running $Label"
  Push-Location $WorkingDirectory
  try {
    & '.\dx11_test_app.exe' @Arguments 2>&1 | Tee-Object -FilePath $logPath
    $exitCode = $LASTEXITCODE
  }
  finally {
    Pop-Location
  }

  if ($exitCode -ne 0) {
    throw "$Label failed with exit code $exitCode. See $logPath"
  }
}

function Assert-CleanRuntimeLog {
  param(
    [string]$Label,
    [string]$Path
  )

  $fatalPatterns = @(
    '\[ERROR\]',
    'CRASHED',
    'DEVICE (ALREADY )?REMOVED',
    'GPU fence wait TIMED OUT'
  )
  # native_fallback makes DXUP's device creation fail on purpose.
  $fatalLines = Select-String -LiteralPath $Path -Pattern $fatalPatterns |
    Where-Object { $_.Line -notmatch 'D3D11CreateDevice failed: DXUP_FAIL_D3D12' }
  if ($fatalLines) {
    $preview = ($fatalLines | Select-Object -First 8 | ForEach-Object { $_.Line }) -join [Environment]::NewLine
    throw "$Label emitted fatal runtime diagnostics despite a zero process exit code:`n$preview`nSee $Path"
  }
}

if ($Build) {
  Invoke-Build
}

foreach ($requiredPath in @($sourceExe, $sourceWrapperD3D11, $sourceWrapperDxgi)) {
  if (-not (Test-Path $requiredPath)) {
    throw "Required build artifact not found: $requiredPath"
  }
}

if (-not (Test-Path $runRoot)) {
  New-Item -ItemType Directory -Path $runRoot | Out-Null
}

if (-not (Test-Path $logDir)) {
  New-Item -ItemType Directory -Path $logDir | Out-Null
}

$appArgs = @()
if ($StressOnly) {
  $appArgs += '--stress-only'
} elseif ($Stress) {
  $appArgs += '--stress'
}

if ($StressIterations -gt 0) {
  $appArgs += '--stress-iterations'
  $appArgs += [string]$StressIterations
}

Stage-SystemRun
Stage-WrapperRun

Invoke-TestRun -Label 'system D3D11 runtime' -WorkingDirectory $systemRunDir -LogName 'system.log' -Arguments $appArgs
Invoke-TestRun -Label 'DXUP wrapper' -WorkingDirectory $wrapperRunDir -LogName 'wrapper.log' -Arguments $appArgs
Assert-CleanRuntimeLog -Label 'DXUP wrapper' -Path (Join-Path $logDir 'wrapper.log')

Write-Host ''
Write-Host 'All DX11 harness runs completed successfully.'
Write-Host "Logs: $logDir"
