# Unit tests for the preset export/import (de)serialization logic
# (src/ptz-preset-io.cpp - see that file's own comment for why it's a
# standalone pair of functions rather than PTZDevice methods). Pure
# OBSData-in/OBSData-out logic against real libobs, with no PTZDevice, no
# ptzDeviceList, no obs-frontend-api, and no running OBS instance needed -
# unlike tests/uart-hil, this needs no hardware either, so it's safe to
# build and run in CI.
#
# Compiles src/ptz-preset-io.cpp/.hpp and src/protocol-helpers.cpp/.hpp
# directly (standalone, not linked from the plugin).

# macOS's cmake/macos/xcode.cmake sets CMAKE_SKIP_RPATH TRUE project-wide -
# silently makes any INSTALL_RPATH/BUILD_RPATH target property below a
# no-op. This is a plain standalone executable (no post-build framework
# embedding step like the plugin bundle gets), so it needs real RPATH
# handling - re-enable it, scoped to this subdirectory only. No-op on
# Linux/Windows (CMAKE_SKIP_RPATH is already FALSE there). Same reasoning
# as tests/uart-hil/CMakeLists.txt.
set(CMAKE_SKIP_RPATH FALSE)

add_executable(
  preset-io-tests
  main.cpp
  test_preset_io.cpp
  test_preset_io_edge_cases.cpp
  "${CMAKE_SOURCE_DIR}/src/ptz-preset-io.hpp"
  "${CMAKE_SOURCE_DIR}/src/ptz-preset-io.cpp"
  "${CMAKE_SOURCE_DIR}/src/protocol-helpers.hpp"
  "${CMAKE_SOURCE_DIR}/src/protocol-helpers.cpp"
)

target_include_directories(preset-io-tests PRIVATE "${CMAKE_SOURCE_DIR}/src")

target_link_libraries(preset-io-tests PRIVATE Qt6::Core Qt6::Widgets OBS::libobs catch2_amalgamated)

set_target_properties(preset-io-tests PROPERTIES AUTOMOC ON CXX_STANDARD 17)

# Same RPATH reasoning as tests/uart-hil/CMakeLists.txt: point directly at
# every CMAKE_PREFIX_PATH entry's Frameworks/lib subdirectory, covering
# libobs (.deps/Frameworks or .deps/.../lib) without hardcoding either path.
if(NOT WIN32)
  set(_preset_io_rpaths "")
  foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
    list(APPEND _preset_io_rpaths "${_prefix}/Frameworks" "${_prefix}/lib")
  endforeach()
  set_target_properties(
    preset-io-tests PROPERTIES BUILD_RPATH "${_preset_io_rpaths}" INSTALL_RPATH "${_preset_io_rpaths}"
  )
endif()
