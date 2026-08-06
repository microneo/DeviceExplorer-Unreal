# Canonical explicit source list shared by the native build. UBT discovers the
# same files from the module directory; do not replace this with file(GLOB).
set(DEVICEEXPLORER_WIRE_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/Private/DeviceExplorerAuthPrimitives.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Private/DeviceExplorerHostManifest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Private/DeviceExplorerHttpUpgrade.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Private/DeviceExplorerJson.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Private/DeviceExplorerMdns.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Private/DeviceExplorerPeerProtocol.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/Private/DeviceExplorerWebSocket.cpp"
)

set(DEVICEEXPLORER_WIRE_HEADERS
    "${CMAKE_CURRENT_LIST_DIR}/Public/DeviceExplorerAuthPrimitives.h"
    "${CMAKE_CURRENT_LIST_DIR}/Public/DeviceExplorerHostManifest.h"
    "${CMAKE_CURRENT_LIST_DIR}/Public/DeviceExplorerHttpUpgrade.h"
    "${CMAKE_CURRENT_LIST_DIR}/Public/DeviceExplorerJson.h"
    "${CMAKE_CURRENT_LIST_DIR}/Public/DeviceExplorerMdns.h"
    "${CMAKE_CURRENT_LIST_DIR}/Public/DeviceExplorerPeerProtocol.h"
    "${CMAKE_CURRENT_LIST_DIR}/Public/DeviceExplorerProtocol.h"
    "${CMAKE_CURRENT_LIST_DIR}/Public/DeviceExplorerWebSocket.h"
)
