cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

# Generate a version number from the git tag that can be embedded in the binary
execute_process(
  COMMAND git describe --tags --dirty
  OUTPUT_VARIABLE _git_version
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  OUTPUT_STRIP_TRAILING_WHITESPACE)
add_compile_definitions(PLUGIN_NAME="${_name}" PLUGIN_VERSION="${_git_version}")

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

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core Svg Network Xml)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets Qt6::Svg Qt6::Network Qt6::Xml)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE src/ptz-controls.ui
          src/settings.ui
          src/ptz-controls.qrc
          src/ptz.c
          src/ptz-controls.cpp
          src/ptz-device.cpp
          src/settings.cpp
          src/ptz-visca.cpp
          src/ptz-visca-udp.cpp
          src/ptz-visca-tcp.cpp
          src/protocol-helpers.cpp
          src/ptz-action-source.c
          src/circularlistview.cpp
          src/touch-control.cpp
          src/imported/qt-wrappers.cpp
          src/imported/obs-app.cpp
          src/imported/properties-view.cpp
          src/imported/vertical-scroll-area.cpp
          src/imported/double-slider.cpp
          src/imported/slider-ignorewheel.cpp
          src/imported/spinbox-ignorewheel.cpp
          src/ptz.h
          src/ptz-controls.hpp
          src/ptz-device.hpp
          src/settings.hpp
          src/ptz-visca.hpp
          src/ptz-visca-udp.hpp
          src/ptz-visca-tcp.hpp
          src/protocol-helpers.hpp
          src/circularlistview.hpp
          src/touch-control.hpp
          src/imported/qt-wrappers.hpp
          src/imported/obs-app.hpp
          src/imported/properties-view.hpp
          src/imported/properties-view.moc.hpp
          src/imported/vertical-scroll-area.hpp
          src/imported/double-slider.hpp
          src/imported/slider-ignorewheel.hpp
          src/imported/spinbox-ignorewheel.hpp)

option(ENABLE_USB_CAM "Enable USB camera support" OFF)
if(ENABLE_USB_CAM)
  add_compile_definitions(ENABLE_USB_CAM)
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/ptz-usb-cam.cpp src/ptz-usb-cam.hpp)
endif()

option(ENABLE_ONVIF "Enable ONVIF camera support" ON)
if(ENABLE_ONVIF)
  add_compile_definitions(ENABLE_ONVIF)
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/ptz-onvif.cpp src/ptz-onvif.hpp)
endif()

option(ENABLE_SERIALPORT "Enable UART connected camera support" OFF)
if(ENABLE_SERIALPORT)
  find_package(Qt${_QT_VERSION} COMPONENTS SerialPort)
  if(Qt${_QT_VERSION}SerialPort_FOUND)
    add_compile_definitions(ENABLE_SERIALPORT)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt::SerialPort)
    target_sources(
      ${CMAKE_PROJECT_NAME}
      PRIVATE src/uart-wrapper.cpp src/uart-wrapper.hpp src/ptz-visca-uart.cpp
              src/ptz-visca-uart.hpp src/ptz-pelco.cpp src/ptz-pelco.hpp)
  endif()
endif()

option(USE_SDL2_STATIC "Use SDL2 static library" ON)
option(ENABLE_JOYSTICK "Enable Joystick support" OFF)
if(ENABLE_JOYSTICK)
  if(USE_SDL2_STATIC)
    find_package(SDL2 COMPONENTS SDL2-static REQUIRED)
    set(SDL2_TARGET SDL2::SDL2-static)
  else()
    find_package(SDL2 REQUIRED)
    set(SDL2_TARGET SDL2::SDL2)
  endif()
  if(SDL2_FOUND)
    add_compile_definitions(ENABLE_JOYSTICK SDL_SUPPORTED)
    target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${SDL2_TARGET})
    target_sources(
      ${CMAKE_PROJECT_NAME}
      PRIVATE src/imported/qjoysticks/QJoysticks.cpp
              src/imported/qjoysticks/QJoysticks.h
              src/imported/qjoysticks/JoysticksCommon.h
              src/imported/qjoysticks/SDL_Joysticks.cpp
              src/imported/qjoysticks/SDL_Joysticks.h
              src/imported/qjoysticks/qjoysticks-res.qrc
              src/imported/qjoysticks/VirtualJoystick.cpp
              src/imported/qjoysticks/VirtualJoystick.h)
  endif()
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
