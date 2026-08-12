# Native host dependencies

The native host uses standalone [Asio](https://think-async.com/Asio/) as its
TCP/UDP event loop. CMake fetches the pinned `asio-1-38-2` source revision when
`DEVICEEXPLORER_ASIO_ROOT` is not supplied. Asio is distributed under the Boost
Software License 1.0; binary packages of the host must carry its license notice.

`DeviceExplorerWire` remains dependency-free and does not include Asio.

The native peer channel uses [Monocypher](https://monocypher.org/) 4.0.2 for
XChaCha20-Poly1305 authenticated encryption and secure key erasure. CMake fetches
the pinned `0d85f98c9d9b0227e42cf795cb527dff372b40a4` revision when
`DEVICEEXPLORER_MONOCYPHER_ROOT` is not supplied. Monocypher is available under
BSD-2-Clause or CC0-1.0; binary packages use the BSD-2-Clause option and must
carry its license notice.
The complete notice is committed as `Native/licenses/Monocypher-BSD-2-Clause.txt`.
