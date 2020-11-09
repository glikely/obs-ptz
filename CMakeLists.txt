project(ptz-controls VERSION 0.1.0)
set(PROJECT_FULL_NAME "PTZ Controls")

find_package(PkgConfig)

pkg_check_modules(LIBVISCA libvisca)
if(LIBVISCA_FOUND)
	add_definitions( -DLIBVISCA_FOUND=${LIBVISCA_FOUND} )
	set(visca_SOURCES ptz-visca.cpp)
	set(visca_HEADERS ptz-visca.hpp)
endif()

set(ptz-controls_HEADERS
    ptz-controls.hpp
    ptz-device.hpp
    ${visca_HEADERS})

set(ptz-controls_SOURCES
    ptz-controls.cpp
    ${visca_SOURCES})

set(ptz-controls_QRC
    ptz-controls.qrc)

if(WIN32)
	get_filename_component(ISS_FILES_DIR "${CMAKE_BINARY_DIR}\\..\\package" ABSOLUTE)
	file(TO_NATIVE_PATH "${ISS_FILES_DIR}" ISS_FILES_DIR)
	get_filename_component(ISS_PACKAGE_DIR "${CMAKE_PACKAGE_PREFIX}\\.." ABSOLUTE)
	file(TO_NATIVE_PATH "${ISS_PACKAGE_DIR}" ISS_PACKAGE_DIR)
	get_filename_component(ISS_SOURCE_DIR "${PROJECT_SOURCE_DIR}" ABSOLUTE)
	file(TO_NATIVE_PATH "${ISS_SOURCE_DIR}" ISS_SOURCE_DIR)
	configure_file("installer.iss.in"
			"${PROJECT_BINARY_DIR}/installer.iss"
		)
	configure_file(resource.rc.in ptz-controls.rc)
	list(APPEND ptz-controls_SOURCES
		ptz-controls.rc)
endif()

qt5_wrap_ui(ptz-controls_UI_HEADERS ptz-controls.ui)
qt5_add_resources(ptz-controls_QRC_SOURCES ${ptz-controls_QRC})

find_package(Qt5Gamepad QUIET)

add_library(ptz-controls MODULE
	${ptz-controls_HEADERS}
	${ptz-controls_SOURCES}
	${ptz-controls_UI_HEADERS}
	${ptz-controls_QRC_SOURCES})

target_link_libraries(ptz-controls
	obs-frontend-api
	Qt5::Widgets
	${Qt5Gamepad_LIBRARIES}
	${LIBVISCA_LIBRARIES}
	libobs)

set_target_properties(ptz-controls PROPERTIES FOLDER "plugins/glikely")
install_obs_plugin_with_data(ptz-controls data)
