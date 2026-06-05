param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [string]$GameDir = "C:\GOG Games\Silent Hill 4"
)

$ErrorActionPreference = "Stop"

$GameDir = $GameDir.Trim().Trim("'").Trim('"')

$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root "bin\$Configuration\dsound.dll"

if (-not (Test-Path -LiteralPath $source)) {
    throw "Build output not found: $source"
}

if (-not (Test-Path -LiteralPath $GameDir)) {
    throw "Silent Hill 4 install directory not found: $GameDir"
}

$destination = Join-Path $GameDir "dsound.dll"
$scriptsDir = Join-Path $GameDir "scripts"

New-Item -ItemType Directory -Force -Path $scriptsDir | Out-Null
Copy-Item -LiteralPath $source -Destination $destination -Force

Write-Host "Deployed $source"
Write-Host "      to $destination"
