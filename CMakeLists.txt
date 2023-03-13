cmake_minimum_required(VERSION 3.16...3.21)

# Generate a version number from the git tag
execute_process(
  COMMAND git describe --tags --dirty
  OUTPUT_VARIABLE PLUGIN_VERSION
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REGEX REPLACE "^v" "" PLUGIN_VERSION ${PLUGIN_VERSION})

if(PLUGIN_VERSION MATCHES "^[vV]?([0-9][.0-9]+)")
  set(SIMPLE_VERSION ${CMAKE_MATCH_1})
else()
  message(FATAL_ERROR "version ${PLUGIN_VERSION} needs to be in semantic form X.Y.Z[*]")
endif()
add_compile_definitions(PLUGIN_NAME="${CMAKE_PROJECT_NAME}" PLUGIN_VERSION="${PLUGIN_VERSION}")

# Change obs-plugintemplate to your plugin's name in a machine-readable format (e.g.:
# obs-myawesomeplugin) and set
project(obs-ptz VERSION ${SIMPLE_VERSION})
add_library(${CMAKE_PROJECT_NAME} MODULE)

# Replace `Your Name Here` with the name (yours or your organization's) you want to see as the
# author of the plugin (in the plugin's metadata itself and in the installers)
set(PLUGIN_AUTHOR "Grant Likely")

# Replace `com.example.obs-plugin-template` with a unique Bundle ID for macOS releases (used both in
# the installer and when submitting the installer for notarization)
set(MACOS_BUNDLEID "ca.secretlab.${CMAKE_PROJECT_NAME}")

# Replace `me@contoso.com` with the maintainer email address you want to put in Linux packages
set(LINUX_MAINTAINER_EMAIL "grant.likely@secretlab.ca")

# Add your custom source files here - header files are optional and only required for visibility
# e.g. in Xcode or Visual Studio
target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE src/ptz-controls.ui
          src/settings.ui
          src/ptz-controls.qrc
          src/ptz.c
          src/ptz-controls.cpp
          src/ptz-device.cpp
          src/settings.cpp
          src/ptz-onvif.cpp
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
          src/ptz-onvif.hpp
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

# Import libobs as main plugin dependency
find_package(libobs REQUIRED)
include(cmake/ObsPluginHelpers.cmake)

# Uncomment these lines if you want to use the OBS Frontend API in your plugin
find_package(obs-frontend-api REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)

# Uncomment those lines if you want to use Qt in your plugin
find_qt(COMPONENTS Widgets Core Svg Network Xml)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt::Core Qt::Widgets Qt::Svg Qt::Network
                                                    Qt::Xml)

option(ENABLE_ONVIF "Enable ONVIF camera support" ON)
if(ENABLE_ONVIF)
  add_compile_definitions(ENABLE_ONVIF)
endif()

option(ENABLE_SERIALPORT "Enable UART connected camera support" ON)
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

option(ENABLE_JOYSTICK "Enable Joystick support" ON)
if(ENABLE_JOYSTICK)
  find_package(SDL2 COMPONENTS SDL2-static)
  if(SDL2_FOUND)
    add_compile_definitions(ENABLE_JOYSTICK SDL_SUPPORTED)
    target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE SDL2::SDL2-static)
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

set_target_properties(
  ${CMAKE_PROJECT_NAME}
  PROPERTIES AUTOMOC ON
             AUTOUIC ON
             AUTORCC ON)

# /!\ TAKE NOTE: No need to edit things past this point /!\

# --- Platform-independent build settings ---

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

# --- End of section ---

# --- Windows-specific build settings and tasks ---
if(OS_WINDOWS)
  configure_file(cmake/bundle/windows/installer-Windows.iss.in
                 ${CMAKE_BINARY_DIR}/installer-Windows.generated.iss)

  configure_file(cmake/bundle/windows/resource.rc.in ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.rc)
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.rc)

  if(MSVC)
    target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE /W4)
  endif()
  # --- End of section ---

  # -- macOS specific build settings and tasks --
elseif(OS_MACOS)
  configure_file(cmake/bundle/macos/installer-macos.pkgproj.in
                 ${CMAKE_BINARY_DIR}/installer-macos.generated.pkgproj)

  set(MACOSX_PLUGIN_GUI_IDENTIFIER "${MACOS_BUNDLEID}")
  set(MACOSX_PLUGIN_BUNDLE_VERSION "${PLUGIN_VERSION}")
  set(MACOSX_PLUGIN_SHORT_VERSION_STRING "1")

  target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE -Wall)
  # --- End of section ---

  # --- Linux-specific build settings and tasks ---
else()
  target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE -Wall)
endif()
# --- End of section ---

setup_plugin_target(${CMAKE_PROJECT_NAME})
