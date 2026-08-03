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
    target_compile_options(dexp-wire PRIVATE /W4 /WX /GR- /EHs-c-)
else()
    target_compile_options(dexp-wire PRIVATE
        -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti
    )
endif()

if(BUILD_TESTING)
    add_executable(dexp-wire-tests
        "${CMAKE_CURRENT_LIST_DIR}/tests/DeviceExplorerWireTests.cpp"
    )
    target_link_libraries(dexp-wire-tests PRIVATE dexp-wire)
    target_compile_features(dexp-wire-tests PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(dexp-wire-tests PRIVATE /W4 /WX /GR- /EHs-c-)
    else()
        target_compile_options(dexp-wire-tests PRIVATE
            -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti
        )
    endif()
    add_test(NAME dexp-wire-tests COMMAND dexp-wire-tests)
endif()
