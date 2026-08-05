param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("client", "server")]
    [string]$Mode,
    [string]$Agent = "build/native/Release/dexp-autobahn-agent.exe"
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$Reports = Join-Path $Root "build/autobahn-reports"
$Image = "crossbario/autobahn-testsuite:25.10.1"
$Container = "deviceexplorer-autobahn-server"

function Wait-TcpPort {
    param(
        [Parameter(Mandatory = $true)] [string] $HostName,
        [Parameter(Mandatory = $true)] [int] $Port
    )

    for ($Attempt = 0; $Attempt -lt 100; ++$Attempt) {
        $Client = [System.Net.Sockets.TcpClient]::new()
        try {
            $Connect = $Client.ConnectAsync($HostName, $Port)
            if ($Connect.Wait(100) -and $Client.Connected) { return }
        }
        catch {
            # The listener is not ready yet.
        }
        finally {
            $Client.Dispose()
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for ${HostName}:${Port}"
}

if (-not (Test-Path $Agent -PathType Leaf)) {
    throw "Autobahn agent not found: $Agent"
}
New-Item -ItemType Directory -Force -Path (Join-Path $Reports "clients"), (Join-Path $Reports "servers") | Out-Null

if ($Mode -eq "client") {
    docker rm -f $Container 2>$null | Out-Null
    docker run --rm -d `
        --name $Container `
        -p 9001:9001 `
        -v "${Root}/Tests/Autobahn:/config:ro" `
        -v "${Reports}:/reports" `
        $Image | Out-Null
    try {
        Wait-TcpPort -HostName "127.0.0.1" -Port 9001
        & $Agent client --host 127.0.0.1 --port 9001 --agent DeviceExplorerWire
        if ($LASTEXITCODE -ne 0) { throw "Autobahn client agent failed" }
    }
    finally {
        docker stop $Container | Out-Null
    }
}
else {
    $Server = Start-Process -FilePath $Agent -ArgumentList "server", "--host", "0.0.0.0", "--port", "9002" -PassThru
    try {
        Wait-TcpPort -HostName "127.0.0.1" -Port 9002
        docker run --rm `
            --add-host "host.docker.internal:host-gateway" `
            -v "${Root}/Tests/Autobahn:/config:ro" `
            -v "${Reports}:/reports" `
            $Image wstest --mode fuzzingclient --spec /config/fuzzingclient.json
        if ($LASTEXITCODE -ne 0) { throw "Autobahn fuzzing client failed" }
    }
    finally {
        Stop-Process -Id $Server.Id -ErrorAction SilentlyContinue
    }
}
