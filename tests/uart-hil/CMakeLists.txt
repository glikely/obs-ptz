# Hardware-in-the-loop test suite for PTZUARTWrapper (src/drivers/serial/uart-wrapper.cpp).
# See README.md for usage. Needs two wired-back-to-back serial ports, or
# `socat` on macOS/Linux for a zero-hardware run - not for CI.
#
# Compiles src/drivers/serial/uart-wrapper.cpp/.hpp directly (standalone, not linked from
# the plugin).

# macOS's cmake/macos/xcode.cmake sets CMAKE_SKIP_RPATH TRUE project-wide -
# silently makes any INSTALL_RPATH/BUILD_RPATH target property below a
# no-op. This is a plain standalone executable (no post-build framework
# embedding step like the plugin bundle gets), so it needs real RPATH
# handling - re-enable it, scoped to this subdirectory only. No-op on
# Linux/Windows (CMAKE_SKIP_RPATH is already FALSE there).
set(CMAKE_SKIP_RPATH FALSE)

add_executable(
  uart-hil-tests
  main.cpp
  obs_module_stub.cpp
  test_config.hpp
  test_config.cpp
  test_harness.hpp
  test_harness.cpp
  socat_harness.hpp
  socat_harness.cpp
  test_data_transfer.cpp
  test_concurrent_rw.cpp
  test_reconnect.cpp
  "${CMAKE_SOURCE_DIR}/src/drivers/serial/uart-wrapper.hpp"
  "${CMAKE_SOURCE_DIR}/src/drivers/serial/uart-wrapper.cpp"
)

target_include_directories(uart-hil-tests PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/src/drivers/serial")

# Same platform split as the plugin itself (see the top-level
# CMakeLists.txt's ENABLE_SERIALPORT block): macOS/Windows link our
# vendored, de-privatized static QSerialPort fork; Linux links real Qt.
if(APPLE OR WIN32)
  set(_uart_hil_serialport_lib qtserialport)
else()
  set(_uart_hil_serialport_lib Qt6::SerialPort)
endif()

target_link_libraries(uart-hil-tests PRIVATE Qt6::Core ${_uart_hil_serialport_lib} OBS::libobs catch2_amalgamated)

set_target_properties(uart-hil-tests PROPERTIES AUTOMOC ON CXX_STANDARD 17)

# Point RPATH directly at every CMAKE_PREFIX_PATH entry's Frameworks/lib
# subdirectory - covers .deps/Frameworks (libobs.framework on macOS, or
# libobs.so on Linux under lib/) and .deps/obs-deps-qt6-*/lib (the Qt
# libs/frameworks) without hardcoding either path.
if(NOT WIN32)
  set(_uart_hil_rpaths "")
  foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
    list(APPEND _uart_hil_rpaths "${_prefix}/Frameworks" "${_prefix}/lib")
  endforeach()
  set_target_properties(uart-hil-tests PROPERTIES BUILD_RPATH "${_uart_hil_rpaths}" INSTALL_RPATH "${_uart_hil_rpaths}")
endif()
