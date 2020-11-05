project(ptz-controls VERSION 0.1.0)
set(PROJECT_FULL_NAME "PTZ Controls")

set(ptz-controls_HEADERS
    ptz-controls.hpp
    ptz-camera.hpp)

set(ptz-controls_SOURCES
    ptz-controls.cpp
    ptz-camera.cpp)

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

add_library(ptz-controls MODULE
	${ptz-controls_HEADERS}
	${ptz-controls_SOURCES}
	${ptz-controls_UI_HEADERS}
	${ptz-controls_QRC_SOURCES})

target_link_libraries(ptz-controls
	obs-frontend-api
	Qt5::Widgets
	libobs
	visca)

set_target_properties(ptz-controls PROPERTIES FOLDER "plugins/glikely")
install_obs_plugin_with_data(ptz-controls data)
