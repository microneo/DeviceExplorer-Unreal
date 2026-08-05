# Native host dependencies

The native host uses standalone [Asio](https://think-async.com/Asio/) as its
TCP/UDP event loop. CMake fetches the pinned `asio-1-38-2` source revision when
`DEVICEEXPLORER_ASIO_ROOT` is not supplied. Asio is distributed under the Boost
Software License 1.0; binary packages of the host must carry its license notice.

`DeviceExplorerWire` remains dependency-free and does not include Asio.
