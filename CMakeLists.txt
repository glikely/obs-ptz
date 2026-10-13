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

add_subdirectory("${CMAKE_SOURCE_DIR}/shared/qt/display" "${CMAKE_BINARY_DIR}/shared/qt/display")
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::qt-display)

# libobs's util/sse-intrin.h always includes SIMDe with SIMDE_ENABLE_NATIVE_ALIASES,
# even on x86. That turns the _mm_*() names into macros, so any real x86
# intrinsics header included *after* <obs.h> (Qt's qsimd.h pulls in
# <immintrin.h>) fails to compile with "redeclared as different kind of
# entity". They are fine when included first. Qt's moc output is compiled as
# one translation unit (mocs_compilation.cpp), so a header that includes
# <obs.h> can be followed by another moc file that includes Qt's headers, and
# no per-file include order inside this project can prevent that. Include the
# real header ahead of everything instead. Other architectures use SIMDe's
# own implementations only, and Windows and macOS builds are unaffected.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i[3-6]86)$")
  target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include immintrin.h>")
endif()

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
  # CMake 3.27+ adds the directory ui_*.h and moc files are generated in as a
  # system include directory, and headers included from a system header are
  # system headers too, which a compiler leaves out of its dependency file.
  # Xcode then doesn't rebuild what includes a widget's header only through
  # ui_*.h when that header changes, and the stale objects have the widget the
  # wrong size. Make it a normal include directory so they are tracked.
  set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES AUTOGEN_USE_SYSTEM_INCLUDE OFF)
endif()

add_dependencies(${CMAKE_PROJECT_NAME} plugin_version)

# The sources are split the way the modules will be: the controller (the
# dock, settings and everything else that talks to devices only through the
# PTZ API) in src/controller, what every driver shares in src/core, and each
# driver in src/drivers/<name>. scripts/check-module-boundaries.py keeps their
# #includes apart. They are all still linked into the one module.

# qt-wrappers is an interface library of sources, which the module compiles
# once; the libraries below only need its header
get_target_property(_ptz_qt_wrappers_include qt-wrappers INTERFACE_INCLUDE_DIRECTORIES)

# What each of the libraries the module links needs: OBS, Qt, and moc,
# set up the way the module itself is above. The core and the serial port are
# static libraries, as each driver module will link its own copy. The drivers
# are object libraries until they are modules: their objects are linked in
# whole, Qt resources included.
function(ptz_add_library target type)
  add_library(${target} ${type})
  target_include_directories(${target} PUBLIC "${CMAKE_SOURCE_DIR}/src" ${_ptz_qt_wrappers_include})
  target_link_libraries(${target} PUBLIC OBS::libobs)
  if(TARGET OBS::obs-frontend-api)
    target_link_libraries(${target} PUBLIC OBS::obs-frontend-api)
  endif()
  if(ENABLE_QT)
    target_link_libraries(${target} PUBLIC Qt6::Core Qt6::Widgets Qt6::Network Qt6::Xml)
    target_compile_options(
      ${target}
      PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
    )
    set_target_properties(
      ${target}
      PROPERTIES AUTOMOC ON AUTORCC ON AUTOGEN_USE_SYSTEM_INCLUDE OFF
    )
  endif()
  set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${target})
endfunction()

# The PTZDevice base class and what the drivers share with it
ptz_add_library(ptz-device-core STATIC)
target_include_directories(ptz-device-core PUBLIC "${CMAKE_SOURCE_DIR}/src/core")
target_sources(
  ptz-device-core
  PRIVATE
    src/core/ptz-device.cpp
    src/core/ptz-device.hpp
    src/core/ptz-lifecycle.hpp
    src/core/ptz-discovery-kind.cpp
    src/core/ptz-discovery-kind.hpp
    src/core/ptz-thumbnail-files.cpp
    src/core/ptz-thumbnail-files.hpp
    src/core/protocol-helpers.cpp
    src/core/protocol-helpers.hpp
)

option(ENABLE_SERIALPORT "Enable UART connected camera support" OFF)
if(ENABLE_SERIALPORT)
  set(_serialport_ok FALSE)
  if(APPLE OR WIN32)
    # macOS/Windows: our own de-privatized, statically-linked QSerialPort
    # fork (see shared/qtserialport/README.md) - real Qt::SerialPort pulls
    # in Qt::CorePrivate, whose ABI breaks across every Qt point release.
    add_subdirectory(shared/qtserialport)
    set(_serialport_lib qtserialport)
    set(_serialport_ok TRUE)
  else()
    # Linux: distro-packaged Qt6::SerialPort. Never had the ABI problem
    # above (obs-ptz and OBS build against the same distro Qt6 there).
    find_package(Qt6 COMPONENTS SerialPort)
    if(Qt6SerialPort_FOUND)
      set(_serialport_lib Qt::SerialPort)
      package_qt_library(${CMAKE_PROJECT_NAME} SerialPort)
      set(_serialport_ok TRUE)
    endif()
  endif()

  if(_serialport_ok)
    add_compile_definitions(ENABLE_SERIALPORT)
    # The serial port, which the VISCA and Pelco drivers share
    ptz_add_library(ptz-serial STATIC)
    target_include_directories(ptz-serial PUBLIC "${CMAKE_SOURCE_DIR}/src/drivers/serial")
    target_sources(ptz-serial PRIVATE src/drivers/serial/uart-wrapper.cpp src/drivers/serial/uart-wrapper.hpp)
    target_link_libraries(ptz-serial PUBLIC ${_serialport_lib})
  endif()
  unset(_serialport_lib)
endif()

ptz_add_library(ptz-driver-visca OBJECT)
target_include_directories(ptz-driver-visca PUBLIC "${CMAKE_SOURCE_DIR}/src/drivers/visca")
target_link_libraries(ptz-driver-visca PUBLIC ptz-device-core)
target_sources(
  ptz-driver-visca
  PRIVATE
    src/drivers/visca/ptz-visca.cpp
    src/drivers/visca/ptz-visca.hpp
    src/drivers/visca/ptz-visca-commands.cpp
    src/drivers/visca/ptz-visca-commands.hpp
    src/drivers/visca/ptz-visca-udp.cpp
    src/drivers/visca/ptz-visca-udp.hpp
    src/drivers/visca/ptz-visca-tcp.cpp
    src/drivers/visca/ptz-visca-tcp.hpp
    src/drivers/visca/ptz-sony-discovery.cpp
    src/drivers/visca/ptz-sony-discovery.hpp
)
if(TARGET ptz-serial)
  target_sources(ptz-driver-visca PRIVATE src/drivers/visca/ptz-visca-uart.cpp src/drivers/visca/ptz-visca-uart.hpp)
  target_link_libraries(ptz-driver-visca PUBLIC ptz-serial)
endif()

# The VISCA command sets shipped with the plugin, linked into it as Qt
# resources, as :/visca-profiles/*.json. A new one is a new file there.
# (qt_add_resources() on a static library makes another object library,
# which the Xcode generator can't link for two architectures.)
file(GLOB VISCA_PROFILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/drivers/visca/visca-profiles/*.json")
qt_add_resources(
  ptz-driver-visca
  visca_profiles
  PREFIX "/visca-profiles"
  BASE "${CMAKE_SOURCE_DIR}/src/drivers/visca/visca-profiles"
  FILES ${VISCA_PROFILES}
)

if(TARGET ptz-serial)
  ptz_add_library(ptz-driver-pelco OBJECT)
  target_include_directories(ptz-driver-pelco PUBLIC "${CMAKE_SOURCE_DIR}/src/drivers/pelco")
  target_link_libraries(ptz-driver-pelco PUBLIC ptz-device-core ptz-serial)
  target_sources(ptz-driver-pelco PRIVATE src/drivers/pelco/ptz-pelco.cpp src/drivers/pelco/ptz-pelco.hpp)
endif()

option(ENABLE_ONVIF "Enable ONVIF camera support" ON)
if(ENABLE_ONVIF)
  add_compile_definitions(ENABLE_ONVIF)
  ptz_add_library(ptz-driver-onvif OBJECT)
  target_include_directories(ptz-driver-onvif PUBLIC "${CMAKE_SOURCE_DIR}/src/drivers/onvif")
  target_link_libraries(ptz-driver-onvif PUBLIC ptz-device-core)
  target_sources(
    ptz-driver-onvif
    PRIVATE
      src/drivers/onvif/ptz-onvif.cpp
      src/drivers/onvif/ptz-onvif.hpp
      src/drivers/onvif/ptz-onvif-discovery.cpp
      src/drivers/onvif/ptz-onvif-discovery.hpp
  )
endif()

option(ENABLE_USB_CAM "Enable USB camera support" ON)
if(ENABLE_USB_CAM)
  # Each platform provides its own backend; see src/drivers/usb/ptz-usb-backend.hpp
  if(APPLE)
    set(_usb_backend_sources src/drivers/usb/ptz-usb-backend-macos.cpp src/drivers/usb/uvc-protocol.hpp)
    set(_usb_backend_libs "-framework IOKit" "-framework CoreFoundation")
  elseif(WIN32)
    set(_usb_backend_sources src/drivers/usb/ptz-usb-backend-windows.cpp)
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_usb_backend_sources src/drivers/usb/ptz-usb-backend-linux.cpp)
  else()
    message(WARNING "USB camera support is not available on ${CMAKE_SYSTEM_NAME}")
    set(ENABLE_USB_CAM OFF)
  endif()
endif()
if(ENABLE_USB_CAM)
  add_compile_definitions(ENABLE_USB_CAM)
  ptz_add_library(ptz-driver-usb OBJECT)
  target_include_directories(ptz-driver-usb PUBLIC "${CMAKE_SOURCE_DIR}/src/drivers/usb")
  target_link_libraries(ptz-driver-usb PUBLIC ptz-device-core ${_usb_backend_libs})
  target_sources(
    ptz-driver-usb
    PRIVATE
      src/drivers/usb/ptz-usb-cam.cpp
      src/drivers/usb/ptz-usb-cam.hpp
      src/drivers/usb/ptz-usb-backend.hpp
      src/drivers/usb/ptz-usb-backend-slot.hpp
      src/drivers/usb/ptz-usb-worker.cpp
      src/drivers/usb/ptz-usb-worker.hpp
      ${_usb_backend_sources}
  )
endif()
unset(_usb_backend_sources)
unset(_usb_backend_libs)

ptz_add_library(ptz-driver-soft OBJECT)
target_include_directories(ptz-driver-soft PUBLIC "${CMAKE_SOURCE_DIR}/src/drivers/soft")
target_link_libraries(ptz-driver-soft PUBLIC ptz-device-core)
target_sources(
  ptz-driver-soft
  PRIVATE src/drivers/soft/ptz-soft-cam.cpp src/drivers/soft/ptz-soft-cam.hpp src/drivers/soft/ptz-soft-viewport.hpp
)

# The controller, and the one file that loads it and the drivers together
# until they are separate modules
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/controller)
target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    ${PLUGIN_VERSION_SOURCE}
    src/ptz.h
    src/ptz-module-all.cpp
    src/controller/ptz-camera-report.ui
    src/controller/ptz-controls.ui
    src/controller/ptz-movement-controls.ui
    src/controller/settings.ui
    src/controller/ptz-controls.qrc
    src/controller/ptz.c
    src/controller/ptz-controls.cpp
    src/controller/ptz-movement-controls.cpp
    src/controller/ptz-thumbnail-sweep.cpp
    src/controller/ptz-device-util.cpp
    src/controller/ptz-legacy-migration.cpp
    src/controller/ptz-list-model.cpp
    src/controller/ptz-driver-kinds.cpp
    src/controller/ptz-backup-store.cpp
    src/controller/settings.cpp
    src/controller/ptz-camera-report.cpp
    src/controller/ptz-state-view.cpp
    src/controller/ptz-source-display.cpp
    src/controller/ptz-ws-decl.cpp
    src/controller/ptz-websocket.cpp
    src/controller/ptz-action-source.c
    src/controller/circularlistview.cpp
    src/controller/touch-control.cpp
    src/controller/ptz-controls.hpp
    src/controller/ptz-movement-controls.hpp
    src/controller/ptz-thumbnail-sweep.hpp
    src/controller/ptz-legacy-migration.hpp
    src/controller/ptz-list-model.hpp
    src/controller/ptz-driver-kinds.hpp
    src/controller/ptz-backup-store.hpp
    src/controller/settings.hpp
    src/controller/ptz-camera-report.hpp
    src/controller/ptz-state-view.hpp
    src/controller/ptz-source-display.hpp
    src/controller/ptz-ws-decl.hpp
    src/controller/ptz-websocket.hpp
    src/controller/obs-websocket-api.h
    src/controller/contributors-generated.hpp
    src/controller/translators-generated.hpp
    src/controller/circularlistview.hpp
    src/controller/touch-control.hpp
    src/controller/square-tool-button.hpp
)

option(
  ENABLE_UART_TESTS
  "Build the hardware-in-the-loop UART test suite (needs two physical serial ports, or socat on macOS/Linux; not for CI)"
  OFF
)
if(ENABLE_UART_TESTS)
  if(NOT ENABLE_SERIALPORT)
    message(
      FATAL_ERROR
      "ENABLE_UART_TESTS requires ENABLE_SERIALPORT=ON (needs libserialport and src/drivers/serial/uart-wrapper.*)"
    )
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
  ENABLE_SOFT_PTZ_TESTS
  "Build the unit tests for the soft PTZ viewport model (needs no OBS, Qt, camera or serial port)"
  OFF
)
if(ENABLE_SOFT_PTZ_TESTS)
  if(NOT TARGET catch2_amalgamated)
    add_subdirectory(shared/catch2)
  endif()
  add_subdirectory(tests/soft-ptz)
endif()

option(ENABLE_UNIT_TESTS "Build the unit tests for OBS- and Qt-free code in src/ (ptz-unit-tests), run by ctest" OFF)
if(ENABLE_UNIT_TESTS)
  if(NOT TARGET catch2_amalgamated)
    add_subdirectory(shared/catch2)
  endif()
  enable_testing()
  add_subdirectory(tests/unit)
endif()

option(ENABLE_PROTOCOL_TRACE "Enable very verbose protocol tracing" OFF)
if(ENABLE_PROTOCOL_TRACE)
  add_compile_definitions(ENABLE_PROTOCOL_TRACE)
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
      tests/ui-harness/appearance-row-sizing-test.cpp
      tests/ui-harness/preset-export-import-test.cpp
      tests/ui-harness/device-status-test.cpp
      tests/ui-harness/move-device-test.cpp
      tests/ui-harness/camera-report-test.cpp
      tests/ui-harness/set-device-test.cpp
      tests/ui-harness/update-device-test.cpp
      tests/ui-harness/preset-view-test.cpp
      tests/ui-harness/reorder-presets-test.cpp
      tests/ui-harness/reorder-cameras-test.cpp
      tests/ui-harness/dock-controls-test.cpp
      tests/ui-harness/scene-item-select-test.cpp
      tests/ui-harness/device-source-test.cpp
      tests/ui-harness/device-settings-test.cpp
      tests/ui-harness/device-state-test.cpp
      tests/ui-harness/device-signals-test.cpp
      tests/ui-harness/settings-dialog-test.cpp
      tests/ui-harness/api-version-test.cpp
      tests/ui-harness/driver-kinds-test.cpp
      tests/ui-harness/discovery-test.cpp
      tests/ui-harness/device-backup-test.cpp
      tests/ui-harness/third-party-device-test.cpp
      tests/ui-harness/vendor-event-test.cpp
  )
endif()

option(
  ENABLE_VISCA_PROFILE_CHECK
  "Check the VISCA command sets in src/drivers/visca/visca-profiles with the plugin's own reader on every build (tests/visca-profile-check)"
  OFF
)
if(ENABLE_VISCA_PROFILE_CHECK)
  add_subdirectory(tests/visca-profile-check)
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
