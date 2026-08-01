[CmdletBinding()]
param(
    [switch] $SkipInstall
)

$ErrorActionPreference = "Stop"
$PluginRoot = Split-Path -Parent $PSScriptRoot
$WebRoot = Join-Path $PluginRoot "WebUI"
$OutputRoot = Join-Path $PluginRoot "Resources\Web"
$NpmCacheRoot = Join-Path ([System.IO.Path]::GetTempPath()) "DeviceExplorer-npm-cache"

if (-not (Get-Command node -ErrorAction SilentlyContinue)) {
    throw "Node.js was not found. Install a current Node.js LTS release to rebuild DeviceExplorer WebUI."
}
if (-not (Get-Command npm -ErrorAction SilentlyContinue)) {
    throw "npm was not found."
}

Push-Location $WebRoot
try {
    if (-not $SkipInstall) {
        npm ci --cache $NpmCacheRoot
        if ($LASTEXITCODE -ne 0) {
            throw "npm ci failed with exit code $LASTEXITCODE."
        }
    }

    npm run build
    if ($LASTEXITCODE -ne 0) {
        throw "WebUI build failed with exit code $LASTEXITCODE."
    }
}
finally {
    Pop-Location
}

$IndexPath = Join-Path $OutputRoot "index.html"
$ManifestPath = Join-Path $OutputRoot "ui-manifest.json"
if (-not (Test-Path $IndexPath) -or -not (Test-Path $ManifestPath)) {
    throw "WebUI build completed without index.html or ui-manifest.json."
}

Write-Host "DeviceExplorer WebUI updated: $OutputRoot" -ForegroundColor Green
