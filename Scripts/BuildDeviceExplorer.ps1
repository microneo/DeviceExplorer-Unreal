[CmdletBinding()]
param(
    [string] $Project,

    [string] $EngineDir = $env:UE_ENGINE_DIR,

    [ValidateSet("Debug", "DebugGame", "Development", "Shipping")]
    [string] $Configuration = "Development",

    [ValidateSet("Win64", "Mac", "Linux")]
    [string] $Platform = "Win64",

    [switch] $Web,
    [switch] $SkipWebInstall
)

$ErrorActionPreference = "Stop"
$PluginRoot = Split-Path $PSScriptRoot -Parent

if ([string]::IsNullOrWhiteSpace($Project)) {
    $ProjectRoot = Split-Path (Split-Path $PluginRoot -Parent) -Parent
    $Candidates = @(Get-ChildItem -LiteralPath $ProjectRoot -Filter "*.uproject" -File)
    if ($Candidates.Count -ne 1) {
        throw "Expected exactly one .uproject in $ProjectRoot. Pass -Project explicitly."
    }
    $Project = $Candidates[0].FullName
}
$Project = (Resolve-Path $Project).Path

if ($Web) {
    & (Join-Path $PSScriptRoot "BuildWebUI.ps1") -SkipInstall:$SkipWebInstall
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

Write-Host "DeviceExplorer host was built for $Platform $Configuration." -ForegroundColor Green
