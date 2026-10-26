param(
  [ValidateSet("Debug", "RelWithDebInfo", "Release", "MinSizeRel")]
  [string]$Configuration = "RelWithDebInfo",
  [switch]$Configure,
  [switch]$InstallPlugin,
  # Also build and run the unit tests: the plugin's (the GoLive answer, scene sync, layout
  # stacking, the HTTP calls) and synqframe-protocol's (the capture-time SEI, SNTP, BPM).
  # A failing test fails the script.
  [switch]$Test,
  # An OBS Studio 32.2.1 source tree, configured with its windows-x64 preset (its .deps
  # included); libobs and obs-frontend-api are built here when missing.
  [string]$ObsSdkRoot,
  [string]$ObsRuntimeRoot
)

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($ObsSdkRoot)) {
  $ObsSdkRoot = Join-Path $repoRoot ".tools\obs-sdk\32.2.1"
}
$obsRoot = (Resolve-Path -LiteralPath $ObsSdkRoot).Path
$obsBuild = Join-Path $obsRoot "build_x64"
$sourceRoot = $repoRoot
$buildRoot = Join-Path $sourceRoot "build_x64"
$cache = Join-Path $buildRoot "CMakeCache.txt"

if (!(Test-Path (Join-Path $obsRoot "CMakeLists.txt"))) {
  throw "The pinned upstream OBS SDK source is unavailable: $obsRoot"
}

if (!(Test-Path (Join-Path $obsBuild "libobs\libobsConfig.cmake"))) {
  & cmake --preset windows-x64 -S $obsRoot
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  & cmake --build $obsBuild --config $Configuration --target libobs obs-frontend-api
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if ($Configure -or !(Test-Path $cache)) {
  $obsPrefixes = @(
    (Join-Path $obsBuild "libobs"),
    (Join-Path $obsBuild "frontend\api"),
    (Join-Path $obsBuild "deps\w32-pthreads"),
    (Join-Path $obsRoot ".deps\obs-deps-2026-07-15-x64"),
    (Join-Path $obsRoot ".deps\obs-deps-qt6-2026-07-15-x64")
  ) -join ";"
  & cmake --fresh -S $sourceRoot -B $buildRoot -G "Visual Studio 18 2026" -A x64 "-DCMAKE_PREFIX_PATH=$obsPrefixes" "-DOBS_SOURCE_DIR=$obsRoot"
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

& cmake --build $buildRoot --config $Configuration --target synqframe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Test) {
  # Target name -> the directory its executable lands in, relative to the build root.
  $tests = [ordered]@{
    "golive-test" = "plugin"
    "scene-sync-test" = "plugin"
    "layout-stacking-test" = "plugin"
    "network-request-test" = "plugin"
    "speech-detector-test" = "plugin\core"
    "capture-time-sei-test" = "vendor\synqframe-protocol"
    "ntp-sample-test" = "vendor\synqframe-protocol"
    "bpm-test" = "vendor\synqframe-protocol"
  }
  & cmake --build $buildRoot --config $Configuration --target ($tests.Keys -join ";").Split(";")
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  # The Qt tests and the HTTP test load Qt's and libcurl's DLLs from the SDK's dependencies.
  $savedPath = $env:PATH
  $env:PATH = (Join-Path $obsRoot ".deps\obs-deps-qt6-2026-07-15-x64\bin") + ";" + (Join-Path $obsRoot ".deps\obs-deps-2026-07-15-x64\bin") + ";" + $env:PATH
  $failed = @()
  foreach ($name in $tests.Keys) {
    $directory = Join-Path $buildRoot (Join-Path $tests[$name] $Configuration)
    $executable = Join-Path $directory "$name.exe"
    if (!(Test-Path $executable)) { throw "Test executable not produced: $executable" }
    Write-Host "== $name"
    & $executable
    if ($LASTEXITCODE -ne 0) { $failed += $name }
  }
  $env:PATH = $savedPath
  if ($failed.Count -gt 0) { throw "Tests failed: $($failed -join ', ')" }
  Write-Host "All tests passed."
}

$plugin = Join-Path $buildRoot "plugin\$Configuration\synqframe.dll"
if (!(Test-Path $plugin)) {
  throw "The Synqframe plugin was not produced."
}

if ($InstallPlugin) {
  if ([string]::IsNullOrWhiteSpace($ObsRuntimeRoot)) {
    $ObsRuntimeRoot = Join-Path $repoRoot ".runtime\obs-portable"
  }
  $ObsRuntimeRoot = (Resolve-Path -LiteralPath $ObsRuntimeRoot).Path
  $runtimeExecutable = Join-Path $ObsRuntimeRoot "bin\64bit\obs64.exe"
  if (!(Test-Path -LiteralPath $runtimeExecutable)) {
    throw "No portable OBS executable found at $runtimeExecutable"
  }
  foreach ($obsProcess in @(Get-Process obs64 -ErrorAction SilentlyContinue)) {
    if ($obsProcess.Path -eq $runtimeExecutable) {
      throw "Close this portable OBS before installing: $ObsRuntimeRoot"
    }
  }
  $pluginDirectory = Join-Path $ObsRuntimeRoot "obs-plugins\64bit"
  New-Item -ItemType Directory -Force -Path $pluginDirectory | Out-Null
  Copy-Item -LiteralPath $plugin -Destination (Join-Path $pluginDirectory "synqframe.dll") -Force -ErrorAction Stop

  $pluginData = Join-Path $repoRoot "plugin\data"
  $pluginDataDirectory = Join-Path $ObsRuntimeRoot "data\obs-plugins\synqframe"
  New-Item -ItemType Directory -Force -Path $pluginDataDirectory | Out-Null
  Copy-Item -Path (Join-Path $pluginData "*") -Destination $pluginDataDirectory -Recurse -Force
}

Write-Host "Synqframe plugin: $plugin"
