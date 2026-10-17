[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release", "Both")]
    [string]$Configuration = "Both",
    [string]$MsBuildPath = $env:MSBUILD_PATH,
    [string]$QtMsBuildPath = $env:QT_MSBUILD,
    [string]$OpenCvRoot = $env:OPENCV_ROOT
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$projectPath = Join-Path $projectRoot "relay_station.vcxproj"

function Select-ExistingPath {
    param([string[]]$Candidates)
    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    return $null
}

$MsBuildPath = Select-ExistingPath @(
    $MsBuildPath,
    "D:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
)
$QtMsBuildPath = Select-ExistingPath @(
    $QtMsBuildPath,
    (Join-Path $env:LOCALAPPDATA "QtMsBuild")
)
$localOpenCv = Join-Path $projectRoot "third_party\opencv"
$OpenCvRoot = Select-ExistingPath @(
    $OpenCvRoot,
    $(if (Test-Path -LiteralPath (Join-Path $localOpenCv "build\include\opencv2\opencv.hpp")) { $localOpenCv }),
    "D:\APP\OpenCV\opencv"
)

if (-not $MsBuildPath) { throw "MSBuild was not found. Set MSBUILD_PATH." }
if (-not $QtMsBuildPath) { throw "QtMsBuild was not found. Set QT_MSBUILD." }
if (-not $OpenCvRoot) { throw "OpenCV SDK was not found. Set OPENCV_ROOT." }
if (-not (Test-Path -LiteralPath $projectPath)) { throw "Project not found: $projectPath" }

$env:QTFRAMEWORK_BYPASS_LICENSE_CHECK = "1"
$configurations = if ($Configuration -eq "Both") { @("Debug", "Release") } else { @($Configuration) }

foreach ($item in $configurations) {
    Write-Host "Building relay_station ($item|x64)..."
    & $MsBuildPath $projectPath "/m" "/t:Rebuild" "/p:Configuration=$item" "/p:Platform=x64" "/p:QtMsBuild=$QtMsBuildPath" "/p:OpenCvRoot=$OpenCvRoot" "/v:minimal"
    if ($LASTEXITCODE -ne 0) {
        throw "relay_station $item build failed with exit code $LASTEXITCODE"
    }

    $exePath = Join-Path $projectRoot "x64\$item\relay_station.exe"
    if (-not (Test-Path -LiteralPath $exePath)) {
        throw "Build reported success but output is missing: $exePath"
    }
    Write-Host "Built: $exePath"
}
