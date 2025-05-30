cmake_minimum_required(VERSION 3.28...3.30)

find_package(Qt6 REQUIRED Core Widgets)

if(NOT TARGET OBS::qt-wrappers)
  add_subdirectory("${CMAKE_SOURCE_DIR}/shared/qt/wrappers" "${CMAKE_BINARY_DIR}/shared/qt/wrappers")
endif()

if(NOT TARGET OBS::qt-plain-text-edit)
  add_subdirectory("${CMAKE_SOURCE_DIR}/shared/qt/plain-text-edit" "${CMAKE_BINARY_DIR}/shared/qt/plain-text-edit")
endif()

if(NOT TARGET OBS::qt-vertical-scroll-area)
  add_subdirectory(
    "${CMAKE_SOURCE_DIR}/shared/qt/vertical-scroll-area"
    "${CMAKE_BINARY_DIR}/shared/qt/vertical-scroll-area"
  )
endif()

if(NOT TARGET OBS::qt-slider-ignorewheel)
  add_subdirectory(
    "${CMAKE_SOURCE_DIR}/shared/qt/slider-ignorewheel"
    "${CMAKE_BINARY_DIR}/shared/qt/slider-ignorewheel"
  )
endif()

if(NOT TARGET OBS::qt-icon-label)
  add_subdirectory("${CMAKE_SOURCE_DIR}/shared/qt/icon-label" "${CMAKE_BINARY_DIR}/shared/qt/icon-label")
endif()

add_library(properties-view INTERFACE)
add_library(OBS::properties-view ALIAS properties-view)

target_sources(
  properties-view
  INTERFACE
    double-slider.cpp
    double-slider.hpp
    properties-view.cpp
    properties-view.hpp
    properties-view.moc.hpp
    spinbox-ignorewheel.cpp
    spinbox-ignorewheel.hpp
)
target_include_directories(properties-view INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(
  properties-view
  INTERFACE
    OBS::obs-frontend-api
    OBS::libobs
    OBS::qt-wrappers
    OBS::qt-plain-text-edit
    OBS::qt-vertical-scroll-area
    OBS::qt-slider-ignorewheel
    OBS::qt-icon-label
    Qt::Core
    Qt::Widgets
)
