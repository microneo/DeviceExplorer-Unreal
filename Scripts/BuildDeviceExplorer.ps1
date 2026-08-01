[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Project,

    [string] $EngineDir = $env:UE_ENGINE_DIR,

    [ValidateSet("Debug", "DebugGame", "Development", "Shipping")]
    [string] $Configuration = "Development",

    [ValidateSet("Win64", "Mac", "Linux")]
    [string] $Platform = "Win64",

    [switch] $SkipWebInstall,
    [switch] $WebOnly
)

$ErrorActionPreference = "Stop"
$Project = (Resolve-Path $Project).Path

& (Join-Path $PSScriptRoot "BuildWebUI.ps1") -SkipInstall:$SkipWebInstall
if ($WebOnly) {
    exit 0
}

if ([string]::IsNullOrWhiteSpace($EngineDir)) {
    throw "Pass -EngineDir or set UE_ENGINE_DIR."
}

& (Join-Path $PSScriptRoot "InstallDeviceExplorerHostTarget.ps1") -Project $Project

$BatchFiles = Join-Path $EngineDir "Engine\Build\BatchFiles"
$BuildScript = if ($Platform -eq "Win64") {
    Join-Path $BatchFiles "Build.bat"
}
else {
    Join-Path $BatchFiles "RunUBT.sh"
}

if (-not (Test-Path $BuildScript)) {
    throw "Unreal build script was not found: $BuildScript"
}

& $BuildScript DeviceExplorerHost $Platform $Configuration "-Project=$Project" -WaitMutex -NoHotReload
if ($LASTEXITCODE -ne 0) {
    throw "DeviceExplorerHost build failed with exit code $LASTEXITCODE."
}

Write-Host "DeviceExplorer WebUI and host were built successfully." -ForegroundColor Green
