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
$Staged = Join-Path $VersionDirectory "DeviceExplorerHost.$PID.tmp"
Copy-Item -Force $Executable $Staged
if ([IO.File]::Exists($InstalledExecutable)) {
    # A running host keeps its own image locked against writes, but Windows still allows
    # that image to be renamed, so reinstalling never has to wait for the old host to exit.
    [IO.File]::Move($InstalledExecutable, "$InstalledExecutable.$([guid]::NewGuid().ToString('N')).old")
}
[IO.File]::Move($Staged, $InstalledExecutable)
foreach ($Stale in Get-ChildItem -Path $VersionDirectory -Filter "*.old" -File) {
    try { Remove-Item -Force $Stale.FullName } catch { }
}

$Pointer = Join-Path $InstallRoot "current.txt"
$TemporaryPointer = "$Pointer.$PID.tmp"
[IO.File]::WriteAllText($TemporaryPointer, "versions/$BuildId/DeviceExplorerHost.exe`n", [Text.UTF8Encoding]::new($false))
if ([IO.File]::Exists($Pointer)) {
    # Windows PowerShell converts a bare $null into an empty string, which Replace
    # rejects as a backup path; [NullString]::Value is the way to ask for no backup.
    [IO.File]::Replace($TemporaryPointer, $Pointer, [NullString]::Value)
} else {
    [IO.File]::Move($TemporaryPointer, $Pointer)
}

Write-Host "Installed DeviceExplorer native host $BuildId in $VersionDirectory." -ForegroundColor Green
