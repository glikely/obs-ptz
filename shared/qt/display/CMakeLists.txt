cmake_minimum_required(VERSION 3.28...3.30)

find_package(Qt6 REQUIRED Core Gui Widgets)

add_library(qt-display INTERFACE)
add_library(OBS::qt-display ALIAS qt-display)

# Vendored from OBS Studio (frontend/widgets/OBSQTDisplay.*, and the two
# frontend/utility headers it uses), which plugins can't link to. Changes
# from OBS 32.2.1's: the #include paths of the two utility headers, and a
# guard that turns the Wayland branch off when the Qt private header it needs
# isn't installed (see OBSQTDisplay.cpp). When Qt's private Gui headers are
# there, link them, and a native Wayland session works too.
target_sources(qt-display INTERFACE OBSQTDisplay.cpp OBSQTDisplay.hpp SurfaceEventFilter.hpp display-helpers.hpp)
target_include_directories(qt-display INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(qt-display INTERFACE OBS::libobs Qt::Core Qt::Gui Qt::Widgets)

find_package(Qt6 OPTIONAL_COMPONENTS GuiPrivate)
if(TARGET Qt6::GuiPrivate)
  target_link_libraries(qt-display INTERFACE Qt6::GuiPrivate)
endif()
