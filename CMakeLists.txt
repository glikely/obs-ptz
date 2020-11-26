cmake_minimum_required(VERSION 3.5)
project(ptz-controls VERSION 0.1.0)
set(PROJECT_FULL_NAME "PTZ Controls")

set(CMAKE_AUTOMOC TRUE)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
#if(QTDIR)
#	set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} ${QT_DIR}/lib/cmake)
#endif()

find_package(Qt5Core REQUIRED)
find_package(Qt5Widgets REQUIRED)
find_package(Qt5Svg REQUIRED)
find_package(Qt5Gamepad REQUIRED)

find_package(LibObs REQUIRED)
if(WIN32)
	find_path(OBS_FRONTEND_INCLUDE_DIRS obs-frontend-api.h HINTS ${LIBOBS_INCLUDE_DIRS}/../UI/obs-frontend-api REQUIRED)
	find_file(OBS_FRONTEND_LIBRARIES obs-frontend-api.lib HINTS ${LibObs_DIR}/../UI/obs-frontend-api/Debug REQUIRED)
endif()

if(NOT WIN32)
	find_package(PkgConfig)
	pkg_check_modules(LIBVISCA libvisca)
	if(LIBVISCA_FOUND)
		add_definitions( -DLIBVISCA_FOUND=${LIBVISCA_FOUND} )
		set(visca_SOURCES ptz-visca.cpp)
		set(visca_HEADERS ptz-visca.hpp)
	endif()
endif()

set(ptz-controls_HEADERS
    ptz-controls.hpp
    ptz-device.hpp
    settings.hpp
    ${visca_HEADERS})

set(ptz-controls_SOURCES
    ptz-controls.cpp
    ptz-device.cpp
    settings.cpp
    ${visca_SOURCES})

set(ptz-controls_QRC
    ptz-controls.qrc)

qt5_wrap_ui(ptz-controls_UI_HEADERS ptz-controls.ui settings.ui)
qt5_add_resources(ptz-controls_QRC_SOURCES ${ptz-controls_QRC})

add_library(ptz-controls MODULE
	${ptz-controls_HEADERS}
	${ptz-controls_SOURCES}
	${ptz-controls_UI_HEADERS}
	${ptz-controls_QRC_SOURCES})

include_directories(${LIBOBS_INCLUDE_DIRS}
	${OBS_FRONTEND_INCLUDE_DIRS}
	${Qt5Core_INCLUDES}
	${Qt5Widgets_INCLUDES})

target_link_libraries(ptz-controls
	${Qt5Widgets_LIBRARIES}
	${Qt5Gamepad_LIBRARIES}
	${LIBOBS_LIBRARIES}
	${OBS_FRONTEND_LIBRARIES}
	${LIBVISCA_LIBRARIES})

set_target_properties(ptz-controls PROPERTIES FOLDER "plugins/glikely")
set_target_properties(ptz-controls PROPERTIES PREFIX "")

# Check for OBS_OUTPUT_DIR. If it exists then it means we're building in the
# obs-studio main source tree. Otherwise it is a standalone build.
if(DEFINED OBS_OUTPUT_DIR)
	install_obs_plugin_with_data(ptz-controls data)
else()
	set(CMAKE_C_FLAGS "-Wall -Wextra -Wvla -Wno-unused-function -Werror-implicit-function-declaration -Wno-missing-braces -Wno-missing-field-initializers ${CMAKE_C_FLAGS} -std=c99 -fno-strict-aliasing")
endif()
