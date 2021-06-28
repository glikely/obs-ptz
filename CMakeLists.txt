cmake_minimum_required(VERSION 3.5)
project(ptz-controls VERSION 0.1.0)
set(PROJECT_FULL_NAME "PTZ Controls")

set(CMAKE_AUTOMOC TRUE)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
if(QTDIR)
	set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} ${QTDIR}/lib/cmake)
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/.git")
	execute_process(COMMAND git describe --always --tags --dirty=-modified
		OUTPUT_VARIABLE OBS_PTZ_VERSION
		WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
		OUTPUT_STRIP_TRAILING_WHITESPACE)
else()
	set(OBS_PTZ_VERSION "unknown")
endif()
add_definitions( -DOBS_PTZ_VERSION=${OBS_PTZ_VERSION} )

find_package(Qt5Core REQUIRED)
find_package(Qt5Widgets REQUIRED)
find_package(Qt5Svg REQUIRED)
find_package(Qt5Gamepad REQUIRED)
find_package(Qt5SerialPort REQUIRED)
find_package(Qt5Network REQUIRED)

find_package(LibObs REQUIRED)
get_filename_component(OBS_FRONTEND_INCLUDE_DIRS "${LIBOBS_INCLUDE_DIRS}/../UI/obs-frontend-api" ABSOLUTE)
if(WIN32)
	find_file(OBS_FRONTEND_LIBRARIES obs-frontend-api.lib HINTS "${LibObs_DIR}/../UI/obs-frontend-api/RelWithDebInfo" REQUIRED)
else()
	set(OBS_FRONTEND_LIBRARIES obs-frontend-api)
endif()

set(ptz-controls_HEADERS
    ptz-controls.hpp
    ptz-device.hpp
    settings.hpp
    ptz-visca.hpp
    ptz-pelco-p.hpp)

set(ptz-controls_SOURCES
    ptz-controls.cpp
    ptz-device.cpp
    settings.cpp
    ptz-visca.cpp
    ptz-pelco-p.cpp)

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
	${Qt5Widgets_INCLUDES}
	${Qt5Network_INCLUDES})

target_link_libraries(ptz-controls
	${Qt5Widgets_LIBRARIES}
	${Qt5Gamepad_LIBRARIES}
	${Qt5SerialPort_LIBRARIES}
	${Qt5Network_LIBRARIES}
	${LIBOBS_LIBRARIES}
	${OBS_FRONTEND_LIBRARIES})

set_target_properties(ptz-controls PROPERTIES FOLDER "plugins/glikely")
set_target_properties(ptz-controls PROPERTIES PREFIX "")

# Check for OBS_OUTPUT_DIR. If it exists then it means we're building in the
# obs-studio main source tree. Otherwise it is a standalone build.
if(DEFINED OBS_OUTPUT_DIR)
	install_obs_plugin_with_data(ptz-controls data)
else()
	set(CMAKE_C_FLAGS "-Wall -Wextra -Wvla -Wno-unused-function -Werror-implicit-function-declaration -Wno-missing-braces -Wno-missing-field-initializers ${CMAKE_C_FLAGS} -std=c99 -fno-strict-aliasing")
endif()
