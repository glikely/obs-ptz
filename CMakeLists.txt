cmake_minimum_required(VERSION 3.16...3.21)

# Generate a version number from the git tag
if(EXISTS "${CMAKE_SOURCE_DIR}/.git")
	execute_process(COMMAND git describe --always --tags --dirty=-modified
		OUTPUT_VARIABLE PLUGIN_VERSION
		WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
		OUTPUT_STRIP_TRAILING_WHITESPACE)
else()
	set(PLUGIN_VERSION "v0.10.4")
endif()
if (PLUGIN_VERSION MATCHES "^v([.0-9]*)")
	set(PLUGIN_SIMPLE_VERSION "${CMAKE_MATCH_1}")
endif()

# Change obs-plugintemplate to your plugin's name in a machine-readable format
# (e.g.: obs-myawesomeplugin) and set
project(obs-ptz VERSION ${PLUGIN_SIMPLE_VERSION})
add_library(${CMAKE_PROJECT_NAME} MODULE)

# Replace `Your Name Here` with the name (yours or your organization's) you want
# to see as the author of the plugin (in the plugin's metadata itself and in the
# installers)
set(PLUGIN_AUTHOR "Grant Likely")

# Replace `com.example.obs-plugin-template` with a unique Bundle ID for macOS
# releases (used both in the installer and when submitting the installer for
# notarization)
set(MACOS_BUNDLEID "ca.secretlab.${CMAKE_PROJECT_NAME}")

# Replace `me@contoso.com` with the maintainer email address you want to put in
# Linux packages
set(LINUX_MAINTAINER_EMAIL "grant.likely@secretlab.ca")

# Add your custom source files here - header files are optional and only
# required for visibility e.g. in Xcode or Visual Studio
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
	src/ptz-controls.ui
	src/settings.ui
	src/ptz-controls.qrc
	src/ptz.c
	src/ptz-controls.cpp
	src/ptz-device.cpp
	src/settings.cpp
	src/ptz-onvif.cpp
	src/ptz-pelco.cpp
	src/ptz-visca.cpp
	src/ptz-visca-uart.cpp
	src/ptz-visca-udp.cpp
	src/ptz-visca-tcp.cpp
	src/ptz-action-source.c
	src/protocol-helpers.cpp
	src/imported/qt-wrappers.cpp
	src/imported/properties-view.cpp
	src/imported/vertical-scroll-area.cpp
	src/imported/double-slider.cpp
	src/imported/slider-ignorewheel.cpp
	src/imported/combobox-ignorewheel.cpp
	src/imported/spinbox-ignorewheel.cpp
	src/ptz.h
	src/ptz-controls.hpp
	src/ptz-device.hpp
	src/settings.hpp
	src/ptz-onvif.hpp
	src/ptz-pelco.hpp
	src/ptz-visca.hpp
	src/ptz-visca-uart.hpp
	src/ptz-visca-udp.hpp
	src/ptz-visca-tcp.hpp
	src/protocol-helpers.hpp
	src/imported/qt-wrappers.hpp
	src/imported/properties-view.hpp
	src/imported/properties-view.moc.hpp
	src/imported/vertical-scroll-area.hpp
	src/imported/double-slider.hpp
	src/imported/slider-ignorewheel.hpp
	src/imported/combobox-ignorewheel.hpp
	src/imported/spinbox-ignorewheel.hpp
)

# /!\ TAKE NOTE: No need to edit things past this point /!\

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
include(cmake/ObsPluginHelpers.cmake)
find_qt(VERSION ${QT_VERSION} COMPONENTS Widgets Core Svg SerialPort Network Xml)

configure_file(src/plugin-macros.h.in
               ${CMAKE_SOURCE_DIR}/src/plugin-macros.generated.h)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-macros.generated.h)

configure_file(
    ci/ci_includes.sh.in
    ../ci/ci_includes.generated.sh
)
configure_file(
    ci/ci_includes.cmd.in
    ../ci/ci_includes.generated.cmd
)

# --- Platform-independent build settings ---

target_include_directories(${CMAKE_PROJECT_NAME}
                           PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(
  ${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs OBS::obs-frontend-api Qt::Core
                                Qt::Widgets Qt::SerialPort Qt::Network
                                Qt::Xml)

set_target_properties(
  ${CMAKE_PROJECT_NAME}
  PROPERTIES AUTOMOC ON
             AUTOUIC ON
             AUTORCC ON)

target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_17)

# --- End of section ---

# --- Windows-specific build settings and tasks ---
if(OS_WINDOWS)
  configure_file(cmake/bundle/windows/installer-Windows.iss.in
                 ${CMAKE_BINARY_DIR}/installer-Windows.generated.iss)

  if(MSVC)
    target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE /MP /d2FH4-)
  endif()
  # --- End of section ---

  # -- macOS specific build settings and tasks --
elseif(OS_MACOS)
  configure_file(cmake/bundle/macos/installer-macOS.pkgproj.in
                 ${CMAKE_BINARY_DIR}/installer-macOS.generated.pkgproj)

  set(MACOSX_PLUGIN_GUI_IDENTIFIER "${MACOS_BUNDLEID}")
  set(MACOSX_PLUGIN_BUNDLE_VERSION "${CMAKE_PROJECT_VERSION}")
  set(MACOSX_PLUGIN_SHORT_VERSION_STRING "1")

  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE -Wall -Wextra -Werror-implicit-function-declaration -stdlib=libc++
            -fvisibility=default)

  set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES PREFIX "")
  # --- End of section ---

  # --- Linux-specific build settings and tasks ---
else()
  target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE -Wall -Wextra)

  set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES PREFIX "")
endif()
# --- End of section ---

setup_plugin_target(${CMAKE_PROJECT_NAME})
