[CmdletBinding()]
param(
    [ValidateSet("default", "offline", "empty", "errors")]
    [string] $Scenario = "default",
    [int] $Port = 5173,
    [switch] $SkipInstall,
    [switch] $Open
)

$ErrorActionPreference = "Stop"
$PluginRoot = Split-Path -Parent $PSScriptRoot
$WebRoot = Join-Path $PluginRoot "WebUI"
$NpmCacheRoot = Join-Path ([System.IO.Path]::GetTempPath()) "DeviceExplorer-npm-cache"
$Mode = if ($Scenario -eq "default") { "mock" } else { "mock-$Scenario" }
$Url = "http://127.0.0.1:$Port"

if (-not (Get-Command node -ErrorAction SilentlyContinue)) {
    throw "Node.js was not found. Install a current Node.js LTS release to run DeviceExplorer WebUI."
}
if (-not (Get-Command npm -ErrorAction SilentlyContinue)) {
    throw "npm was not found."
}

Push-Location $WebRoot
try {
    if (-not $SkipInstall -and -not (Test-Path (Join-Path $WebRoot "node_modules"))) {
        npm ci --cache $NpmCacheRoot
        if ($LASTEXITCODE -ne 0) {
            throw "npm ci failed with exit code $LASTEXITCODE."
        }
    }

    Write-Host "DeviceExplorer WebUI mock ($Scenario) on $Url" -ForegroundColor Green
    Write-Host "No Unreal Engine or DeviceExplorerHost required. Press Ctrl+C to stop." -ForegroundColor DarkGray

    if ($Open) {
        Start-Process $Url | Out-Null
    }

    npx vite --mode $Mode --port $Port
    if ($LASTEXITCODE -ne 0) {
        throw "Vite dev server exited with code $LASTEXITCODE."
    }
}
finally {
    Pop-Location
}
