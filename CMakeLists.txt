cmake_minimum_required(VERSION 3.10)

# Change obs-plugintemplate to your plugin's name in a machine-readable format
# (e.g.: obs-myawesomeplugin) and set 
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
project(ptz-controls VERSION ${PLUGIN_SIMPLE_VERSION})

set(PLUGIN_FULL_NAME "PTZ controller OBS Plugin")

# Replace `Your Name Here` with the name (yours or your organization's) you want
# to see as the author of the plugin (in the plugin's metadata itself and in the installers)
set(PLUGIN_AUTHOR "Grant Likely")

# Replace `com.example.obs-plugin-template` with a unique Bundle ID for macOS releases
# (used both in the installer and when submitting the installer for notarization)
set(MACOS_BUNDLEID "ca.secretlab.ca.ptz-controls")

# Replace `me@contoso.com` with the maintainer email address you want to put in Linux packages
set(LINUX_MAINTAINER_EMAIL "grant.likely@secretlab.ca")

# TAKE NOTE: No need to edit things past this point

set(CMAKE_PREFIX_PATH "${QTDIR}")
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

# In case you need C++
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (WIN32 OR APPLE)
include(external/FindLibObs.cmake)
endif()

find_package(LibObs REQUIRED)
find_package(Qt5 REQUIRED COMPONENTS Core Widgets Svg SerialPort Network)
if(NOT WIN32)
	find_package(Qt5Gamepad)
	if(DEFINED Qt5Gamepad_LIBRARIES AND NOT WIN32)
		set(QT_USE_GAMEPAD "#define CONFIG_USE_GAMEPAD 1")
	endif()
endif()

configure_file(
	config.h.in
	config.generated.h
)
configure_file(
    installer/installer-macOS.pkgproj.in
    ../installer/installer-macOS.generated.pkgproj
)
configure_file(
    installer/installer-Windows.iss.in
    ../installer/installer-Windows.generated.iss
)

configure_file(
    ci/ci_includes.sh.in
    ../ci/ci_includes.generated.sh
)
configure_file(
    ci/ci_includes.cmd.in
    ../ci/ci_includes.generated.cmd
)

set(PLUGIN_SOURCES
	ptz.c
	ptz-controls.cpp
	ptz-device.cpp
	settings.cpp
	ptz-pelco.cpp
	ptz-visca.cpp
	ptz-visca-uart.cpp
	ptz-visca-udp.cpp
	ptz-visca-tcp.cpp
	ptz-action-source.c
	protocol-helpers.cpp
	imported/qt-wrappers.cpp
	imported/properties-view.cpp
	imported/vertical-scroll-area.cpp
	imported/double-slider.cpp
	imported/slider-ignorewheel.cpp
	imported/combobox-ignorewheel.cpp
	imported/spinbox-ignorewheel.cpp
)

set(PLUGIN_HEADERS
	config.generated.h
	ptz.h
	ptz-controls.hpp
	ptz-device.hpp
	settings.hpp
	ptz-pelco.hpp
	ptz-visca.hpp
	ptz-visca-uart.hpp
	ptz-visca-udp.hpp
	ptz-visca-tcp.hpp
	protocol-helpers.hpp
	imported/qt-wrappers.hpp
	imported/properties-view.hpp
	imported/properties-view.moc.hpp
	imported/vertical-scroll-area.hpp
	imported/double-slider.hpp
	imported/slider-ignorewheel.hpp
	imported/combobox-ignorewheel.hpp
	imported/spinbox-ignorewheel.hpp
)

set(PLUGIN_QRC ptz-controls.qrc)

qt5_wrap_ui(PLUGIN_UI_HEADERS ptz-controls.ui settings.ui)
qt5_add_resources(PLUGIN_QRC_SOURCES ${PLUGIN_QRC})

# --- Platform-independent build settings ---
add_library(${CMAKE_PROJECT_NAME} MODULE ${PLUGIN_SOURCES} ${PLUGIN_HEADERS}
	${PLUGIN_UI_HEADERS}
	${PLUGIN_QRC_SOURCES})

include_directories(
	"${LIBOBS_INCLUDE_DIR}/../UI/obs-frontend-api"
	${Qt5Core_INCLUDES}
	${Qt5Widgets_INCLUDES}
	${Qt5Network_INCLUDES}
)

target_link_libraries(${CMAKE_PROJECT_NAME}
	libobs
	Qt5::Core
	Qt5::Widgets
	Qt5::SerialPort
	Qt5::Network
	${Qt5Gamepad_LIBRARIES}
)

# --- End of section ---

# --- Windows-specific build settings and tasks ---
if(WIN32)
	if(NOT DEFINED OBS_FRONTEND_LIB)
		set(OBS_FRONTEND_LIB "OBS_FRONTEND_LIB-NOTFOUND" CACHE FILEPATH "OBS frontend library")
		message(FATAL_ERROR "Could not find OBS Frontend API\'s library !")
	endif()

    # Enable Multicore Builds and disable FH4 (to not depend on VCRUNTIME140_1.DLL when building with VS2019)
    if (MSVC)
        add_definitions(/MP /d2FH4-)
    endif()

	if(CMAKE_SIZEOF_VOID_P EQUAL 8)
		set(ARCH_NAME "64bit")
		set(OBS_BUILDDIR_ARCH "build64")
	else()
		set(ARCH_NAME "32bit")
		set(OBS_BUILDDIR_ARCH "build32")
	endif()

	include_directories(
		"${LIBOBS_INCLUDE_DIR}/../${OBS_BUILDDIR_ARCH}/UI"
	)

	target_link_libraries(${CMAKE_PROJECT_NAME}
		"${OBS_FRONTEND_LIB}"
    )

	# --- Release package helper ---
	# The "release" folder has a structure similar OBS' one on Windows
	set(RELEASE_DIR "${PROJECT_SOURCE_DIR}/release")

    add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
        # If config is Release or RelWithDebInfo, package release files
        COMMAND if $<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>==1 (
            "${CMAKE_COMMAND}" -E make_directory
                "${RELEASE_DIR}/data/obs-plugins/${CMAKE_PROJECT_NAME}"
                "${RELEASE_DIR}/obs-plugins/${ARCH_NAME}"
        )

        COMMAND if $<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>==1 (
            "${CMAKE_COMMAND}" -E copy_directory
                "${PROJECT_SOURCE_DIR}/data"
                "${RELEASE_DIR}/data/obs-plugins/${CMAKE_PROJECT_NAME}"
        )

        COMMAND if $<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>==1 (
            "${CMAKE_COMMAND}" -E copy
                "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>"
                "${RELEASE_DIR}/obs-plugins/${ARCH_NAME}"
        )

        COMMAND if $<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>==1 (
            "${CMAKE_COMMAND}" -E copy
	        "$<SHELL_PATH:${QTDIR}/bin/Qt5Gamepad.dll>"
	        "$<SHELL_PATH:${QTDIR}/bin/Qt5SerialPort.dll>"
                "${RELEASE_DIR}/obs-plugins/${ARCH_NAME}"
        )

        # If config is RelWithDebInfo, copy the pdb file
        COMMAND if $<CONFIG:RelWithDebInfo>==1 (
            "${CMAKE_COMMAND}" -E copy
                "$<TARGET_PDB_FILE:${CMAKE_PROJECT_NAME}>"
                "${RELEASE_DIR}/obs-plugins/${ARCH_NAME}"
        )

        # Copy to obs-studio dev environment for immediate testing
        COMMAND if $<CONFIG:Debug>==1 (
            "${CMAKE_COMMAND}" -E copy
                "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>"
                "${LIBOBS_INCLUDE_DIR}/../${OBS_BUILDDIR_ARCH}/rundir/$<CONFIG>/obs-plugins/${ARCH_NAME}"
        )

        COMMAND if $<CONFIG:Debug>==1 (
            "${CMAKE_COMMAND}" -E copy
                "$<TARGET_PDB_FILE:${CMAKE_PROJECT_NAME}>"
                "${LIBOBS_INCLUDE_DIR}/../${OBS_BUILDDIR_ARCH}/rundir/$<CONFIG>/obs-plugins/${ARCH_NAME}"
        )

        COMMAND if $<CONFIG:Debug>==1 (
            "${CMAKE_COMMAND}" -E copy
	        "$<SHELL_PATH:${QTDIR}/bin/Qt5Gamepadd.dll>"
	        "$<SHELL_PATH:${QTDIR}/bin/Qt5SerialPortd.dll>"
                "${LIBOBS_INCLUDE_DIR}/../${OBS_BUILDDIR_ARCH}/rundir/$<CONFIG>/obs-plugins/${ARCH_NAME}"
        )

        COMMAND if $<CONFIG:Debug>==1 (
            "${CMAKE_COMMAND}" -E make_directory
                "${LIBOBS_INCLUDE_DIR}/../${OBS_BUILDDIR_ARCH}/rundir/$<CONFIG>/data/obs-plugins/${CMAKE_PROJECT_NAME}"
        )

        COMMAND if $<CONFIG:Debug>==1 (
            "${CMAKE_COMMAND}" -E copy_directory
                "${PROJECT_SOURCE_DIR}/data"
                "${LIBOBS_INCLUDE_DIR}/../${OBS_BUILDDIR_ARCH}/rundir/$<CONFIG>/data/obs-plugins/${CMAKE_PROJECT_NAME}"
        )
    )
	# --- End of sub-section ---

endif()
# --- End of section ---

# --- Linux-specific build settings and tasks ---
if(UNIX AND NOT APPLE)
    include(GNUInstallDirs)

	set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES PREFIX "")
	target_link_libraries(${CMAKE_PROJECT_NAME} obs-frontend-api)

	file(GLOB locale_files data/locale/*.ini)

	install(TARGETS ${CMAKE_PROJECT_NAME}
		LIBRARY DESTINATION "${CMAKE_INSTALL_FULL_LIBDIR}/obs-plugins")

	install(FILES ${locale_files}
		DESTINATION "${CMAKE_INSTALL_FULL_DATAROOTDIR}/obs/obs-plugins/${CMAKE_PROJECT_NAME}/locale")
endif()
# --- End of section ---

# -- OS X specific build settings and tasks --
if(APPLE)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++ -fvisibility=default")

	set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES PREFIX "")
	target_link_libraries(${CMAKE_PROJECT_NAME} "${OBS_FRONTEND_LIB}")
endif()
# -- End of section --
