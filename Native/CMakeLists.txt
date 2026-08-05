cmake_minimum_required(VERSION 3.20)
project(DeviceExplorerNative VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include("${CMAKE_CURRENT_LIST_DIR}/../Source/DeviceExplorerWire/sources.cmake")

add_library(dexp-wire STATIC
    ${DEVICEEXPLORER_WIRE_SOURCES}
    ${DEVICEEXPLORER_WIRE_HEADERS}
)
target_include_directories(dexp-wire PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/../Source/DeviceExplorerWire/Public"
)
target_compile_features(dexp-wire PUBLIC cxx_std_17)

if(MSVC)
    target_compile_definitions(dexp-wire PRIVATE _HAS_EXCEPTIONS=0)
    target_compile_options(dexp-wire PRIVATE /W4 /WX /GR- /EHs-c-)
else()
    target_compile_options(dexp-wire PRIVATE
        -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti
    )
endif()

option(DEVICEEXPLORER_BUILD_HOST "Build the standalone native host" ON)
set(DEVICEEXPLORER_ASIO_ROOT "" CACHE PATH "Existing standalone Asio source root (the directory containing include/asio.hpp)")
set(DEVICEEXPLORER_BUILD_ID_OVERRIDE "" CACHE STRING "Stable build id for source archives without Git metadata")
if(DEVICEEXPLORER_BUILD_HOST)
    find_package(Threads REQUIRED)
    if(DEVICEEXPLORER_ASIO_ROOT)
        set(DEVICEEXPLORER_ASIO_INCLUDE_DIR "${DEVICEEXPLORER_ASIO_ROOT}/include")
    else()
        include(FetchContent)
        FetchContent_Declare(deviceexplorer_asio
            GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
            GIT_TAG 8806a6803cde7054c3049d3666d3ec36786568c5
            GIT_PROGRESS FALSE
        )
        FetchContent_GetProperties(deviceexplorer_asio)
        if(NOT deviceexplorer_asio_POPULATED)
            FetchContent_Populate(deviceexplorer_asio)
        endif()
        set(DEVICEEXPLORER_ASIO_INCLUDE_DIR "${deviceexplorer_asio_SOURCE_DIR}/include")
    endif()
    if(NOT EXISTS "${DEVICEEXPLORER_ASIO_INCLUDE_DIR}/asio.hpp")
        message(FATAL_ERROR "Standalone Asio was not found below ${DEVICEEXPLORER_ASIO_INCLUDE_DIR}")
    endif()

    add_library(dexp-asio INTERFACE)
    target_include_directories(dexp-asio SYSTEM INTERFACE "${DEVICEEXPLORER_ASIO_INCLUDE_DIR}")
    target_compile_definitions(dexp-asio INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
    if(WIN32)
        target_compile_definitions(dexp-asio INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
    endif()
    target_link_libraries(dexp-asio INTERFACE Threads::Threads)

    add_library(dexp-host-core STATIC
        "${CMAKE_CURRENT_LIST_DIR}/host/DeviceExplorerHostCore.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/host/DeviceExplorerHostCore.h"
        "${CMAKE_CURRENT_LIST_DIR}/host/DeviceExplorerHostIdentity.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/host/DeviceExplorerHostIdentity.h"
        "${CMAKE_CURRENT_LIST_DIR}/host/DeviceExplorerHostMdns.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/host/DeviceExplorerHostMdns.h"
        "${CMAKE_CURRENT_LIST_DIR}/host/DeviceExplorerHostRuntime.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/host/DeviceExplorerHostRuntime.h"
    )
    target_include_directories(dexp-host-core PUBLIC "${CMAKE_CURRENT_LIST_DIR}/host")
    target_link_libraries(dexp-host-core PUBLIC dexp-wire PRIVATE dexp-asio)
    if(WIN32)
        target_link_libraries(dexp-host-core PRIVATE iphlpapi)
    endif()
    target_compile_features(dexp-host-core PUBLIC cxx_std_17)

    set(DEVICEEXPLORER_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    set(DEVICEEXPLORER_BUILD_ID_HEADER "${DEVICEEXPLORER_GENERATED_DIR}/DeviceExplorerBuildId.h")
    add_custom_target(dexp-build-id
        COMMAND "${CMAKE_COMMAND}"
            "-DDEVICEEXPLORER_SOURCE_ROOT=${CMAKE_CURRENT_LIST_DIR}/.."
            "-DDEVICEEXPLORER_BUILD_ID_HEADER=${DEVICEEXPLORER_BUILD_ID_HEADER}"
            "-DDEVICEEXPLORER_BUILD_ID_HEADER_DIR=${DEVICEEXPLORER_GENERATED_DIR}"
            "-DDEVICEEXPLORER_BUILD_ID_OVERRIDE=${DEVICEEXPLORER_BUILD_ID_OVERRIDE}"
            -P "${CMAKE_CURRENT_LIST_DIR}/cmake/GenerateBuildId.cmake"
        BYPRODUCTS "${DEVICEEXPLORER_BUILD_ID_HEADER}"
        VERBATIM
    )

    add_executable(dexp-host "${CMAKE_CURRENT_LIST_DIR}/host/DeviceExplorerHostMain.cpp")
    add_dependencies(dexp-host dexp-build-id)
    target_include_directories(dexp-host PRIVATE "${DEVICEEXPLORER_GENERATED_DIR}")
    target_link_libraries(dexp-host PRIVATE dexp-host-core)
    target_compile_features(dexp-host PRIVATE cxx_std_17)
    if(APPLE)
        set(DEVICEEXPLORER_HOST_INFO_PLIST "${DEVICEEXPLORER_GENERATED_DIR}/DeviceExplorerHostInfo.plist")
        file(MAKE_DIRECTORY "${DEVICEEXPLORER_GENERATED_DIR}")
        configure_file(
            "${CMAKE_CURRENT_LIST_DIR}/host/DeviceExplorerHostInfo.plist.in"
            "${DEVICEEXPLORER_HOST_INFO_PLIST}"
            @ONLY
        )
        # A command-line Mach-O has no bundle plist for macOS local-network privacy.
        # Embed the same metadata section that Xcode's INFOPLIST_FILE setting creates.
        target_link_options(dexp-host PRIVATE
            "LINKER:-sectcreate,__TEXT,__info_plist,${DEVICEEXPLORER_HOST_INFO_PLIST}"
        )
    endif()

    foreach(DEVICEEXPLORER_HOST_TARGET dexp-host-core dexp-host)
        if(MSVC)
            target_compile_options(${DEVICEEXPLORER_HOST_TARGET} PRIVATE /W4 /WX)
        else()
            target_compile_options(${DEVICEEXPLORER_HOST_TARGET} PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endif()
    endforeach()

    if(BUILD_TESTING)
        add_executable(dexp-host-integration-tests
            "${CMAKE_CURRENT_LIST_DIR}/tests/DeviceExplorerHostIntegrationTests.cpp"
        )
        target_link_libraries(dexp-host-integration-tests PRIVATE dexp-host-core dexp-asio)
        target_compile_features(dexp-host-integration-tests PRIVATE cxx_std_17)
        if(MSVC)
            target_compile_options(dexp-host-integration-tests PRIVATE /W4 /WX)
        else()
            target_compile_options(dexp-host-integration-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endif()
        add_test(NAME dexp-host-integration-tests COMMAND dexp-host-integration-tests)
        add_executable(dexp-host-identity-tests
            "${CMAKE_CURRENT_LIST_DIR}/tests/DeviceExplorerHostIdentityTests.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/host/DeviceExplorerHostIdentity.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/host/DeviceExplorerHostIdentity.h"
        )
        target_include_directories(dexp-host-identity-tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/host")
        target_link_libraries(dexp-host-identity-tests PRIVATE dexp-wire)
        target_compile_features(dexp-host-identity-tests PRIVATE cxx_std_17)
        if(MSVC)
            target_compile_options(dexp-host-identity-tests PRIVATE /W4 /WX)
        else()
            target_compile_options(dexp-host-identity-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endif()
        add_test(NAME dexp-host-identity-tests COMMAND dexp-host-identity-tests)
        add_test(NAME dexp-host-version-json COMMAND dexp-host --version-json)
        set_tests_properties(dexp-host-version-json PROPERTIES
            PASS_REGULAR_EXPRESSION "\"manifest_version\":1"
        )
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            add_test(NAME dexp-host-production-smoke
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_LIST_DIR}/../Scripts/RunNativeHostSmoke.py"
                    "$<TARGET_FILE:dexp-host>"
                    --source-root "${CMAKE_CURRENT_LIST_DIR}/.."
            )
            set_tests_properties(dexp-host-production-smoke PROPERTIES TIMEOUT 45)
        endif()
    endif()
endif()

option(DEVICEEXPLORER_BUILD_AUTOBAHN_AGENT "Build the Autobahn WebSocket test adapter" ON)
if(DEVICEEXPLORER_BUILD_AUTOBAHN_AGENT)
    find_package(Threads REQUIRED)
    add_executable(dexp-autobahn-agent
        "${CMAKE_CURRENT_LIST_DIR}/autobahn/DeviceExplorerAutobahnAgent.cpp"
    )
    target_link_libraries(dexp-autobahn-agent PRIVATE dexp-wire Threads::Threads)
    target_compile_features(dexp-autobahn-agent PRIVATE cxx_std_17)
    if(WIN32)
        target_link_libraries(dexp-autobahn-agent PRIVATE ws2_32)
    endif()
    if(MSVC)
        target_compile_definitions(dexp-autobahn-agent PRIVATE _HAS_EXCEPTIONS=0)
        target_compile_options(dexp-autobahn-agent PRIVATE /W4 /WX /GR- /EHs-c-)
    else()
        target_compile_options(dexp-autobahn-agent PRIVATE
            -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti
        )
    endif()
    if(BUILD_TESTING)
        add_test(NAME dexp-autobahn-self-test COMMAND dexp-autobahn-agent self-test)
    endif()
endif()

if(BUILD_TESTING)
    add_executable(dexp-wire-tests
        "${CMAKE_CURRENT_LIST_DIR}/tests/DeviceExplorerWireTests.cpp"
    )
    target_link_libraries(dexp-wire-tests PRIVATE dexp-wire)
    target_compile_features(dexp-wire-tests PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_definitions(dexp-wire-tests PRIVATE _HAS_EXCEPTIONS=0)
        target_compile_options(dexp-wire-tests PRIVATE /W4 /WX /GR- /EHs-c-)
    else()
        target_compile_options(dexp-wire-tests PRIVATE
            -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti
        )
    endif()
    add_test(NAME dexp-wire-tests COMMAND dexp-wire-tests)
endif()
