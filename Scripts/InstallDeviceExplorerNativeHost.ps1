[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Executable,

    [string] $InstallRoot
)

$ErrorActionPreference = "Stop"
$Executable = (Resolve-Path $Executable).Path
if ([string]::IsNullOrWhiteSpace($InstallRoot)) {
    if ([string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
        throw "LOCALAPPDATA is not set; pass -InstallRoot explicitly."
    }
    $InstallRoot = Join-Path $env:LOCALAPPDATA "DeviceExplorer/Host"
}

$Manifest = (& $Executable --version-json | ConvertFrom-Json)
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($Manifest.build_id)) {
    throw "The native host did not return a valid build id."
}
$BuildId = $Manifest.build_id -replace '[^A-Za-z0-9._-]', '_'
$VersionDirectory = Join-Path $InstallRoot "versions/$BuildId"
$InstalledExecutable = Join-Path $VersionDirectory "DeviceExplorerHost.exe"
New-Item -ItemType Directory -Force $VersionDirectory | Out-Null
Copy-Item -Force $Executable $InstalledExecutable

$Pointer = Join-Path $InstallRoot "current.txt"
$TemporaryPointer = "$Pointer.$PID.tmp"
[IO.File]::WriteAllText($TemporaryPointer, "versions/$BuildId/DeviceExplorerHost.exe`n", [Text.UTF8Encoding]::new($false))
if ([IO.File]::Exists($Pointer)) {
    [IO.File]::Replace($TemporaryPointer, $Pointer, $null)
} else {
    [IO.File]::Move($TemporaryPointer, $Pointer)
}

Write-Host "Installed DeviceExplorer native host $BuildId in $VersionDirectory." -ForegroundColor Green
