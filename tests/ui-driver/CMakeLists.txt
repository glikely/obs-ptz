# obs-ui-driver: lets a client of obs-websocket find and drive OBS's Qt
# widgets. See README.md.
#
# Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# A plugin of its own, and not obs-ptz's: it uses nothing of this
# repository's but libobs, obs-frontend-api and Qt, which the top level
# finds. The template's set_target_properties_plugin() is for the one plugin
# a project makes (it packages it, and gives the project's target its
# Windows resources), so what it sets for a plugin to load is set here
# instead.

add_library(obs-ui-driver MODULE)

target_sources(
  obs-ui-driver
  PRIVATE
    ui-driver.cpp
    ui-driver.hpp
    ui-driver-objects.cpp
    ui-driver-objects.hpp
    ui-driver-reading.cpp
    obs-websocket-api.h
)

# The frontend API only for OBS saying it is exiting, which is when the UI
# starts being taken apart
target_link_libraries(obs-ui-driver PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Widgets)

set_target_properties(obs-ui-driver PROPERTIES AUTOMOC ON PREFIX "")

# Next to obs-ptz's, where the scripts that install it look
if(OS_MACOS)
  set_target_properties(
    obs-ui-driver
    PROPERTIES
      BUNDLE TRUE
      BUNDLE_EXTENSION plugin
      XCODE_ATTRIBUTE_PRODUCT_NAME obs-ui-driver
      XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${MACOS_BUNDLEID}.ui-driver
      XCODE_ATTRIBUTE_GENERATE_INFOPLIST_FILE YES
      XCODE_ATTRIBUTE_INFOPLIST_FILE ""
      XCODE_ATTRIBUTE_INFOPLIST_KEY_CFBundleDisplayName obs-ui-driver
  )
  add_custom_command(
    TARGET obs-ui-driver
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/rundir/$<CONFIG>"
    COMMAND
      "${CMAKE_COMMAND}" -E copy_directory "$<TARGET_BUNDLE_DIR:obs-ui-driver>"
      "${CMAKE_BINARY_DIR}/rundir/$<CONFIG>/$<TARGET_BUNDLE_DIR_NAME:obs-ui-driver>"
    COMMENT "Copy obs-ui-driver to rundir"
    VERBATIM
  )
elseif(OS_WINDOWS)
  add_custom_command(
    TARGET obs-ui-driver
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/rundir/$<CONFIG>"
    COMMAND
      "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:obs-ui-driver>"
      "$<$<CONFIG:Debug,RelWithDebInfo,Release>:$<TARGET_PDB_FILE:obs-ui-driver>>"
      "${CMAKE_BINARY_DIR}/rundir/$<CONFIG>"
    COMMENT "Copy obs-ui-driver to rundir"
    VERBATIM
  )
else()
  set_target_properties(obs-ui-driver PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()
