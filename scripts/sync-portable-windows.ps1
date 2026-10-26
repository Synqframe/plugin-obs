param(
  [string]$Source = "C:\Program Files\obs-studio",
  [ValidateSet("Debug", "RelWithDebInfo", "Release", "MinSizeRel")]
  [string]$Configuration = "RelWithDebInfo"
)

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$portableRoot = Join-Path $repoRoot ".runtime\obs-portable"
$sourceRoot = (Resolve-Path -LiteralPath $Source -ErrorAction Stop).Path
$sourceExe = Join-Path $sourceRoot "bin\64bit\obs64.exe"
if (!(Test-Path -LiteralPath $sourceExe)) {
  throw "The source is not an OBS installation: $sourceRoot"
}

New-Item -ItemType Directory -Force -Path $portableRoot | Out-Null
Copy-Item -Path (Join-Path $sourceRoot "*") -Destination $portableRoot -Recurse -Force
New-Item -ItemType File -Force -Path (Join-Path $portableRoot "portable_mode.txt") | Out-Null

$plugin = Join-Path $repoRoot "build_x64\plugins\synqframe\$Configuration\synqframe.dll"
if (Test-Path -LiteralPath $plugin) {
  $pluginDirectory = Join-Path $portableRoot "obs-plugins\64bit"
  New-Item -ItemType Directory -Force -Path $pluginDirectory | Out-Null
  Copy-Item -LiteralPath $plugin -Destination (Join-Path $pluginDirectory "synqframe.dll") -Force
}

Write-Host "Portable OBS: $portableRoot"
Write-Host "Run manually with: $portableRoot\bin\64bit\obs64.exe --portable"
