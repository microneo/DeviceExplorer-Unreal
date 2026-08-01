[CmdletBinding()]
param(
    [string] $Project,
    [switch] $Force
)

$ErrorActionPreference = "Stop"
$PluginRoot = Split-Path $PSScriptRoot -Parent
$PluginDescriptor = Join-Path $PluginRoot "DeviceExplorer.uplugin"
$TemplatePath = Join-Path $PluginRoot "Templates\DeviceExplorerHost.Target.cs"

if (-not (Test-Path -LiteralPath $PluginDescriptor -PathType Leaf)) {
    throw "DeviceExplorer.uplugin was not found next to the Scripts directory."
}
if (-not (Test-Path -LiteralPath $TemplatePath -PathType Leaf)) {
    throw "Host target template was not found: $TemplatePath"
}

if ([string]::IsNullOrWhiteSpace($Project)) {
    $PluginsRoot = Split-Path $PluginRoot -Parent
    $ProjectRoot = Split-Path $PluginsRoot -Parent
    $Candidates = @(Get-ChildItem -LiteralPath $ProjectRoot -Filter "*.uproject" -File)

    if ($Candidates.Count -eq 0) {
        throw "No .uproject was found in $ProjectRoot. Copy DeviceExplorer to <Project>\Plugins\DeviceExplorer or pass -Project explicitly."
    }
    if ($Candidates.Count -gt 1) {
        throw "Multiple .uproject files were found in $ProjectRoot. Pass -Project explicitly."
    }

    $ProjectPath = $Candidates[0].FullName
}
else {
    $ProjectPath = (Resolve-Path -LiteralPath $Project).Path
    if ([System.IO.Path]::GetExtension($ProjectPath) -ne ".uproject") {
        throw "Project must point to a .uproject file: $ProjectPath"
    }
    $ProjectRoot = Split-Path $ProjectPath -Parent
}

$SourceRoot = Join-Path $ProjectRoot "Source"
$TargetPath = Join-Path $SourceRoot "DeviceExplorerHost.Target.cs"
$Installed = $false

New-Item -ItemType Directory -Force -Path $SourceRoot | Out-Null

if (Test-Path -LiteralPath $TargetPath -PathType Leaf) {
    $Current = Get-Content -LiteralPath $TargetPath -Raw
    $Expected = Get-Content -LiteralPath $TemplatePath -Raw
    if ($Current -ne $Expected) {
        if (-not $Force) {
            throw "A different DeviceExplorerHost.Target.cs already exists: $TargetPath. Re-run with -Force to overwrite it."
        }
        Copy-Item -LiteralPath $TemplatePath -Destination $TargetPath -Force
        $Installed = $true
    }
}
else {
    Copy-Item -LiteralPath $TemplatePath -Destination $TargetPath
    $Installed = $true
}

$Status = if ($Installed) { "Installed" } else { "Already installed" }
Write-Host "$Status DeviceExplorerHost target:" -ForegroundColor Green
Write-Host "  $TargetPath"
Write-Host ""
Write-Host "Next steps:"
Write-Host "  1. Regenerate Unreal Engine project files."
Write-Host "  2. Build DeviceExplorerHost or run Scripts\BuildDeviceExplorer.ps1."
