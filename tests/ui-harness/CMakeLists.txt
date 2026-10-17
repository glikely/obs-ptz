# obs-ptz-ui-harness: drives integration tests from inside OBS, for what a
# client outside it can't reach. See README.md.
#
# Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# A plugin of its own, and not obs-ptz's: it uses nothing of obs-ptz's but
# src/ptz.h, for the PTZ API's version, and finds devices as any controller
# does. The template's set_target_properties_plugin() is for the one plugin a
# project makes, so what it sets for a plugin to load is set here instead, as
# for tests/ui-driver/.

add_library(obs-ptz-ui-harness MODULE)

target_sources(
  obs-ptz-ui-harness
  PRIVATE
    ui-test-harness.cpp
    ui-test-harness.hpp
    appearance-row-sizing-test.cpp
    device-settings-test.cpp
    discovery-test.cpp
    third-party-device-test.cpp
    vendor-event-test.cpp
)

# ptz.h, and obs-websocket-api.h, which the UI driver vendors
target_include_directories(obs-ptz-ui-harness PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/tests/ui-driver")

target_link_libraries(obs-ptz-ui-harness PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Widgets)

set_target_properties(obs-ptz-ui-harness PROPERTIES AUTOMOC ON PREFIX "")

# Next to obs-ptz's, where the scripts that install it look
if(OS_MACOS)
  set_target_properties(
    obs-ptz-ui-harness
    PROPERTIES
      BUNDLE TRUE
      BUNDLE_EXTENSION plugin
      XCODE_ATTRIBUTE_PRODUCT_NAME obs-ptz-ui-harness
      XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${MACOS_BUNDLEID}.ui-harness
      XCODE_ATTRIBUTE_GENERATE_INFOPLIST_FILE YES
      XCODE_ATTRIBUTE_INFOPLIST_FILE ""
      XCODE_ATTRIBUTE_INFOPLIST_KEY_CFBundleDisplayName obs-ptz-ui-harness
  )
  add_custom_command(
    TARGET obs-ptz-ui-harness
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/rundir/$<CONFIG>"
    COMMAND
      "${CMAKE_COMMAND}" -E copy_directory "$<TARGET_BUNDLE_DIR:obs-ptz-ui-harness>"
      "${CMAKE_BINARY_DIR}/rundir/$<CONFIG>/$<TARGET_BUNDLE_DIR_NAME:obs-ptz-ui-harness>"
    COMMENT "Copy obs-ptz-ui-harness to rundir"
    VERBATIM
  )
elseif(OS_WINDOWS)
  add_custom_command(
    TARGET obs-ptz-ui-harness
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/rundir/$<CONFIG>"
    COMMAND
      "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:obs-ptz-ui-harness>"
      "$<$<CONFIG:Debug,RelWithDebInfo,Release>:$<TARGET_PDB_FILE:obs-ptz-ui-harness>>"
      "${CMAKE_BINARY_DIR}/rundir/$<CONFIG>"
    COMMENT "Copy obs-ptz-ui-harness to rundir"
    VERBATIM
  )
else()
  set_target_properties(obs-ptz-ui-harness PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()
