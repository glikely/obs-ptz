cmake_minimum_required(VERSION 3.28...3.30)

find_package(Qt6 REQUIRED Core)
option(ENABLE_SDL2_STATIC "Use SDL2 static library" OFF)
if (ENABLE_SDL2_STATIC)
  find_package(SDL2 COMPONENTS SDL2-static REQUIRED)
  set(SDL2_TARGET SDL2::SDL2-static)
else()
  find_package(SDL2 REQUIRED)
  set(SDL2_TARGET SDL2::SDL2)
endif()

add_library(qjoysticks INTERFACE)
target_sources(
  qjoysticks
  INTERFACE QJoysticks.cpp
            QJoysticks.h
            qjoysticks-res.qrc
            QJoysticks/JoysticksCommon.h
            QJoysticks/SDL_Joysticks.cpp
            QJoysticks/SDL_Joysticks.h
            QJoysticks/VirtualJoystick.cpp
            QJoysticks/VirtualJoystick.h)
target_include_directories(qjoysticks INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(qjoysticks INTERFACE ${SDL2_TARGET} Qt::Core)
