param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$workspaceRoot = Split-Path -Parent $PSScriptRoot
$qtRoot = "D:\APP\QT\6.11.0\msvc2022_64"
$qmakePath = Join-Path $qtRoot "bin\qmake.exe"
$vsShellPath = "D:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\Launch-VsDevShell.ps1"
$msbuildPath = "D:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
$outputDirectory = Join-Path $workspaceRoot "x64\OfflineTests"
$projectPath = Join-Path $outputDirectory "relay_core_tests.vcxproj"
$testExecutable = Join-Path $outputDirectory ($Configuration.ToLowerInvariant() + "\relay_core_tests.exe")

foreach ($requiredPath in @($qmakePath, $vsShellPath, $msbuildPath)) {
    if (-not (Test-Path -LiteralPath $requiredPath)) {
        throw "Required build tool was not found: $requiredPath"
    }
}

New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
. $vsShellPath -Arch amd64 -HostArch amd64 -SkipAutomaticLocation -NoLogo
$env:QTFRAMEWORK_BYPASS_LICENSE_CHECK = "1"

& $qmakePath -tp vc -spec win32-msvc (Join-Path $PSScriptRoot "relay_core_tests.pro") -o $projectPath
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $msbuildPath $projectPath /m /t:Build "/p:Configuration=$Configuration" /p:Platform=x64 /v:minimal
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$env:PATH = (Join-Path $qtRoot "bin") + ";" + $env:PATH
& $testExecutable
exit $LASTEXITCODE
