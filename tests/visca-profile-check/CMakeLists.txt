# Checks the VISCA command sets in src/visca-profiles with the plugin's own
# reader, and fails the build if any is one the plugin couldn't read, or isn't
# what a shipped one must be. The plugin itself only leaves out one it can't
# read, with a warning in the log. See main.cpp.

# A plain executable, so its RPATH is wanted (see tests/usb-backend)
set(CMAKE_SKIP_RPATH FALSE)

add_executable(
  visca-profile-check
  main.cpp
  "${CMAKE_SOURCE_DIR}/src/ptz-visca-commands.cpp"
  "${CMAKE_SOURCE_DIR}/src/ptz-visca-commands.hpp"
  "${CMAKE_SOURCE_DIR}/src/protocol-helpers.cpp"
  "${CMAKE_SOURCE_DIR}/src/protocol-helpers.hpp"
)
target_include_directories(visca-profile-check PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(
  visca-profile-check
  PRIVATE OBS::libobs OBS::obs-frontend-api OBS::qt-wrappers Qt6::Core Qt6::Gui Qt6::Widgets
)
set_target_properties(visca-profile-check PROPERTIES AUTOMOC ON CXX_STANDARD 17)

if(NOT WIN32)
  set(_check_rpaths "")
  foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
    list(APPEND _check_rpaths "${_prefix}/Frameworks" "${_prefix}/lib")
  endforeach()
  set_target_properties(visca-profile-check PROPERTIES BUILD_RPATH "${_check_rpaths}")
endif()

# Every build, once the checker is built and whenever a command set changes,
# and with it the ones in bad/, which check the checks
file(GLOB _bad_profiles CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/bad/*.json")
add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/visca-profiles.checked"
  COMMAND visca-profile-check "${CMAKE_SOURCE_DIR}/src/visca-profiles" --expect-errors "${CMAKE_CURRENT_SOURCE_DIR}/bad"
  COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_CURRENT_BINARY_DIR}/visca-profiles.checked"
  DEPENDS visca-profile-check ${VISCA_PROFILES} ${_bad_profiles}
  COMMENT "Checking the VISCA command sets"
  VERBATIM
)
add_custom_target(check-visca-profiles ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/visca-profiles.checked")
