[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [switch]$SkipBuild,
    [string]$VsDevShellPath = $env:VS_DEV_SHELL,
    [string]$QtRoot = $env:QT_ROOT,
    [string]$OpenCvRoot = $env:OPENCV_ROOT
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot

function Select-ExistingPath {
    param([string[]]$Candidates)
    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    return $null
}

$QtRoot = Select-ExistingPath @($QtRoot, "D:\APP\QT\6.11.0\msvc2022_64")
$VsDevShellPath = Select-ExistingPath @(
    $VsDevShellPath,
    "D:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\Launch-VsDevShell.ps1",
    "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Launch-VsDevShell.ps1"
)
$localOpenCv = Join-Path $projectRoot "third_party\opencv"
$OpenCvRoot = Select-ExistingPath @(
    $OpenCvRoot,
    $(if (Test-Path -LiteralPath (Join-Path $localOpenCv "build\x64\vc16\bin")) { $localOpenCv }),
    "D:\APP\OpenCV\opencv"
)
if (-not $QtRoot) { throw "Qt runtime was not found. Set QT_ROOT." }
if (-not $VsDevShellPath) { throw "Visual Studio developer shell was not found. Set VS_DEV_SHELL." }
if (-not $OpenCvRoot) { throw "OpenCV runtime was not found. Set OPENCV_ROOT." }

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "build.ps1") -Configuration $Configuration -OpenCvRoot $OpenCvRoot
}

$sourceExe = Join-Path $projectRoot "x64\$Configuration\relay_station.exe"
$deployDir = Join-Path $projectRoot "deploy\$Configuration"
$windeployqt = Join-Path $QtRoot "bin\windeployqt.exe"
$opencvDllName = if ($Configuration -eq "Debug") { "opencv_world4120d.dll" } else { "opencv_world4120.dll" }
$opencvDll = Join-Path $OpenCvRoot "build\x64\vc16\bin\$opencvDllName"

foreach ($required in @($sourceExe, $windeployqt, $opencvDll)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Required deployment input is missing: $required" }
}

New-Item -ItemType Directory -Path $deployDir -Force | Out-Null
Copy-Item -LiteralPath $sourceExe -Destination (Join-Path $deployDir "relay_station.exe") -Force
Copy-Item -LiteralPath (Join-Path $projectRoot "relay_config.json") -Destination $deployDir -Force
Copy-Item -LiteralPath $opencvDll -Destination $deployDir -Force
Copy-Item -LiteralPath (Join-Path $projectRoot "THIRD_PARTY_NOTICES.md") -Destination $deployDir -Force

$deployMode = if ($Configuration -eq "Debug") { "--debug" } else { "--release" }
. $VsDevShellPath -Arch amd64 -HostArch amd64 -SkipAutomaticLocation -NoLogo
& $windeployqt $deployMode "--compiler-runtime" "--no-translations" (Join-Path $deployDir "relay_station.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed with exit code $LASTEXITCODE" }

$vcRedistX64 = Join-Path $env:VCToolsRedistDir "x64"
$vcCrtDirectory = if (Test-Path -LiteralPath $vcRedistX64) {
    Get-ChildItem -LiteralPath $vcRedistX64 -Directory -Filter "Microsoft.VC*.CRT" |
        Sort-Object Name -Descending |
        Select-Object -First 1
} else {
    $null
}
if (-not $vcCrtDirectory) {
    throw "The x64 MSVC runtime directory was not found after loading the Visual Studio developer shell."
}
Get-ChildItem -LiteralPath $vcCrtDirectory.FullName -File -Filter "*.dll" |
    Copy-Item -Destination $deployDir -Force

$requiredRuntimeFiles = @(
    "relay_station.exe",
    "relay_config.json",
    $opencvDllName,
    "Qt6Core.dll",
    "Qt6Gui.dll",
    "Qt6Network.dll",
    "Qt6Widgets.dll",
    "platforms\qwindows.dll",
    "msvcp140.dll",
    "vcruntime140.dll",
    "vcruntime140_1.dll"
)
foreach ($relativePath in $requiredRuntimeFiles) {
    $fullPath = Join-Path $deployDir $relativePath
    if (-not (Test-Path -LiteralPath $fullPath)) {
        throw "Deployment verification failed; required file is missing: $fullPath"
    }
}

Write-Host "Portable runtime directory created: $deployDir"
Write-Host "The program was packaged but not started."
