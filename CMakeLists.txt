cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${PLUGIN_VERSION})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

add_subdirectory("${CMAKE_SOURCE_DIR}/shared/properties-view" "${CMAKE_BINARY_DIR}/shared/properties-view")
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::properties-view)

if(ENABLE_QT)
  # AGL was removed from Xcode 15+, but the prebuilt libraries ask for it
  if(APPLE)
    foreach(
      _qt_target
      IN
      ITEMS
        Qt6::Core
        Qt6::Widgets
        Qt6::Svg
        Qt6::Network
        Qt6::Xml
        Qt6::SerialPort
        WrapOpenGL::WrapOpenGL
        OpenGL::GL
        OpenGL::GLU
    )
      if(TARGET ${_qt_target})
        foreach(_qt_prop IN ITEMS INTERFACE_LINK_LIBRARIES INTERFACE_LINK_OPTIONS)
          get_target_property(_qt_prop_value ${_qt_target} ${_qt_prop})
          if(_qt_prop_value)
            set(_qt_prop_cleaned "${_qt_prop_value}")
            string(REPLACE "-framework;AGL;" "" _qt_prop_cleaned "${_qt_prop_cleaned}")
            string(REPLACE ";-framework;AGL" "" _qt_prop_cleaned "${_qt_prop_cleaned}")
            string(REPLACE "-framework;AGL" "" _qt_prop_cleaned "${_qt_prop_cleaned}")
            string(REPLACE "-framework AGL" "" _qt_prop_cleaned "${_qt_prop_cleaned}")
            string(REPLACE "/System/Library/Frameworks/AGL.framework" "" _qt_prop_cleaned "${_qt_prop_cleaned}")
            if(NOT _qt_prop_cleaned STREQUAL _qt_prop_value)
              set_property(TARGET ${_qt_target} PROPERTY ${_qt_prop} "${_qt_prop_cleaned}")
            endif()
          endif()
        endforeach()
      endif()
    endforeach()
  endif()
  find_package(
    Qt6
    COMPONENTS Widgets Core Svg Network Xml
  )
  target_link_libraries(
    ${CMAKE_PROJECT_NAME}
    PRIVATE Qt6::Core Qt6::Widgets Qt6::Svg Qt6::Network Qt6::Xml
  )
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

add_dependencies(${CMAKE_PROJECT_NAME} plugin_version)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    ${PLUGIN_VERSION_SOURCE}
    src/ptz-controls.ui
    src/settings.ui
    src/ptz-controls.qrc
    src/ptz.c
    src/ptz-controls.cpp
    src/ptz-device.cpp
    src/ptz-thumbnail.cpp
    src/ptz-list-model.cpp
    src/ptz-discovery.cpp
    src/settings.cpp
    src/ptz-state-view.cpp
    src/ptz-visca.cpp
    src/ptz-visca-udp.cpp
    src/ptz-visca-tcp.cpp
    src/protocol-helpers.cpp
    src/ptz-action-source.c
    src/circularlistview.cpp
    src/touch-control.cpp
    src/ptz.h
    src/ptz-controls.hpp
    src/ptz-device.hpp
    src/ptz-thumbnail.hpp
    src/ptz-list-model.hpp
    src/ptz-discovery.hpp
    src/settings.hpp
    src/ptz-state-view.hpp
    src/contributors-generated.hpp
    src/translators-generated.hpp
    src/ptz-visca.hpp
    src/ptz-visca-udp.hpp
    src/ptz-visca-tcp.hpp
    src/protocol-helpers.hpp
    src/circularlistview.hpp
    src/touch-control.hpp
    src/square-tool-button.hpp
)

option(ENABLE_USB_CAM "Enable USB camera support" ON)
if(ENABLE_USB_CAM)
  # Each platform provides its own backend; see src/ptz-usb-backend.hpp
  if(APPLE)
    set(_usb_backend_sources src/ptz-usb-backend-macos.cpp src/uvc-protocol.hpp)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE "-framework IOKit" "-framework CoreFoundation")
  elseif(WIN32)
    set(_usb_backend_sources src/ptz-usb-backend-windows.cpp)
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_usb_backend_sources src/ptz-usb-backend-linux.cpp)
  else()
    message(WARNING "USB camera support is not available on ${CMAKE_SYSTEM_NAME}")
    set(ENABLE_USB_CAM OFF)
  endif()
endif()
if(ENABLE_USB_CAM)
  add_compile_definitions(ENABLE_USB_CAM)
  target_sources(
    ${CMAKE_PROJECT_NAME}
    PRIVATE
      src/ptz-usb-cam.cpp
      src/ptz-usb-cam.hpp
      src/ptz-usb-backend.hpp
      src/ptz-usb-backend-slot.hpp
      src/ptz-usb-worker.cpp
      src/ptz-usb-worker.hpp
      ${_usb_backend_sources}
  )
  unset(_usb_backend_sources)
endif()

option(ENABLE_ONVIF "Enable ONVIF camera support" ON)
if(ENABLE_ONVIF)
  add_compile_definitions(ENABLE_ONVIF)
  target_sources(
    ${CMAKE_PROJECT_NAME}
    PRIVATE src/ptz-onvif.cpp src/ptz-onvif.hpp src/ptz-onvif-discovery.cpp src/ptz-onvif-discovery.hpp
  )
endif()

option(ENABLE_SERIALPORT "Enable UART connected camera support" OFF)
if(ENABLE_SERIALPORT)
  set(_serialport_ok FALSE)
  if(APPLE OR WIN32)
    # macOS/Windows: our own de-privatized, statically-linked QSerialPort
    # fork (see shared/qtserialport/README.md) - real Qt::SerialPort pulls
    # in Qt::CorePrivate, whose ABI breaks across every Qt point release.
    add_subdirectory(shared/qtserialport)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE qtserialport)
    set(_serialport_ok TRUE)
  else()
    # Linux: distro-packaged Qt6::SerialPort. Never had the ABI problem
    # above (obs-ptz and OBS build against the same distro Qt6 there).
    find_package(Qt6 COMPONENTS SerialPort)
    if(Qt6SerialPort_FOUND)
      target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt::SerialPort)
      package_qt_library(${CMAKE_PROJECT_NAME} SerialPort)
      set(_serialport_ok TRUE)
    endif()
  endif()

  if(_serialport_ok)
    add_compile_definitions(ENABLE_SERIALPORT)
    target_sources(
      ${CMAKE_PROJECT_NAME}
      PRIVATE
        src/uart-wrapper.cpp
        src/uart-wrapper.hpp
        src/ptz-visca-uart.cpp
        src/ptz-visca-uart.hpp
        src/ptz-pelco.cpp
        src/ptz-pelco.hpp
    )
  endif()
  unset(_serialport_ok)
endif()

option(
  ENABLE_UART_TESTS
  "Build the hardware-in-the-loop UART test suite (needs two physical serial ports, or socat on macOS/Linux; not for CI)"
  OFF
)
if(ENABLE_UART_TESTS)
  if(NOT ENABLE_SERIALPORT)
    message(FATAL_ERROR "ENABLE_UART_TESTS requires ENABLE_SERIALPORT=ON (needs libserialport and src/uart-wrapper.*)")
  endif()
  if(NOT TARGET catch2_amalgamated)
    add_subdirectory(shared/catch2)
  endif()
  add_subdirectory(tests/uart-hil)
endif()

option(
  ENABLE_USB_TESTS
  "Build the unit tests for the USB camera backend and worker thread (needs no camera or serial port)"
  OFF
)
if(ENABLE_USB_TESTS)
  if(NOT TARGET catch2_amalgamated)
    add_subdirectory(shared/catch2)
  endif()
  if(NOT TARGET Qt6::Core)
    find_package(Qt6 REQUIRED COMPONENTS Core)
  endif()
  add_subdirectory(tests/usb-backend)
  if(APPLE)
    # Needs a camera; run by tests/usb-e2e/run-macos.sh
    add_subdirectory(tests/usb-e2e)
  endif()
endif()

option(
  ENABLE_UI_TESTS
  "Link in the UI test harness (tests/ui-harness), driven over obs-websocket; inert unless PTZ_UI_TEST_HARNESS=1 is also set at runtime. See tests/ui-harness/README.md."
  OFF
)
if(ENABLE_UI_TESTS)
  add_compile_definitions(ENABLE_UI_TESTS)
  target_sources(
    ${CMAKE_PROJECT_NAME}
    PRIVATE
      tests/ui-harness/ui-test-harness.cpp
      tests/ui-harness/ui-test-harness.hpp
      tests/ui-harness/obs-websocket-api.h
      tests/ui-harness/appearance-row-sizing-test.cpp
      tests/ui-harness/preset-export-import-test.cpp
      tests/ui-harness/device-status-test.cpp
      tests/ui-harness/move-device-test.cpp
      tests/ui-harness/set-device-test.cpp
      tests/ui-harness/update-device-test.cpp
      tests/ui-harness/preset-view-test.cpp
      tests/ui-harness/device-source-test.cpp
      tests/ui-harness/device-settings-test.cpp
      tests/ui-harness/device-state-test.cpp
      tests/ui-harness/device-signals-test.cpp
      tests/ui-harness/settings-dialog-test.cpp
      tests/ui-harness/api-version-test.cpp
      tests/ui-harness/device-backup-test.cpp
  )
endif()

option(ENABLE_JOYSTICK "Enable Joystick support" OFF)
if(ENABLE_JOYSTICK)
  add_subdirectory("${CMAKE_SOURCE_DIR}/shared/qjoysticks" "${CMAKE_BINARY_DIR}/shared/qjoysticks")
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE qjoysticks)
  add_compile_definitions(ENABLE_JOYSTICK SDL_SUPPORTED)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

if(OS_WINDOWS)
  configure_file(installer-Windows.iss.in ${CMAKE_BINARY_DIR}/installer-Windows.iss)
endif()
