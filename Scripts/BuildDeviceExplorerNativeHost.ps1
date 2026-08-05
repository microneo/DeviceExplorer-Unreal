[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string] $Configuration = "Release",

    [string] $BuildDir,

    [string] $AsioRoot,

    [switch] $SkipTests
)

$ErrorActionPreference = "Stop"
$PluginRoot = Split-Path $PSScriptRoot -Parent
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $PluginRoot "build/native-host"
}

$ConfigureArguments = @(
    "-S", (Join-Path $PluginRoot "Native"),
    "-B", $BuildDir,
    "-DCMAKE_BUILD_TYPE=$Configuration",
    "-DDEVICEEXPLORER_BUILD_HOST=ON"
)
if (-not [string]::IsNullOrWhiteSpace($AsioRoot)) {
    $ConfigureArguments += "-DDEVICEEXPLORER_ASIO_ROOT=$AsioRoot"
}

& cmake @ConfigureArguments
if ($LASTEXITCODE -ne 0) {
    throw "Native host configure failed with exit code $LASTEXITCODE."
}
& cmake --build $BuildDir --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) {
    throw "Native host build failed with exit code $LASTEXITCODE."
}
if (-not $SkipTests) {
    & ctest --test-dir $BuildDir -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) {
        throw "Native host tests failed with exit code $LASTEXITCODE."
    }
}

Write-Host "DeviceExplorer native host built in $BuildDir." -ForegroundColor Green
