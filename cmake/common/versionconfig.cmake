# OBS CMake common version helper module

include_guard(GLOBAL)

set(_plugin_version ${_plugin_default_version})
set(_plugin_version_canonical ${_plugin_default_version})

# Attempt to automatically discover expected OBS version
if(NOT DEFINED PLUGIN_VERSION_OVERRIDE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
  execute_process(
    COMMAND git describe --always --tags --dirty=-modified
    OUTPUT_VARIABLE _plugin_version
    ERROR_VARIABLE _git_describe_err
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    RESULT_VARIABLE _plugin_version_result
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )

  if(_git_describe_err)
    message(FATAL_ERROR "Could not fetch OBS version tag from git.\n" ${_git_describe_err})
  endif()

  if(_plugin_version_result EQUAL 0)
    string(REGEX REPLACE "v?([0-9]+)\\.([0-9]+)\\.([0-9]+).*" "\\1;\\2;\\3" _plugin_version_canonical ${_plugin_version})
  endif()
elseif(DEFINED PLUGIN_VERSION_OVERRIDE)
  if(PLUGIN_VERSION_OVERRIDE MATCHES "v?([0-9]+)\\.([0-9]+)\\.([0-9]+).*")
    string(
      REGEX REPLACE
      "v?([0-9]+)\\.([0-9]+)\\.([0-9]+).*"
      "\\1;\\2;\\3"
      _plugin_version_canonical
      ${PLUGIN_VERSION_OVERRIDE}
    )
    set(_plugin_version ${PLUGIN_VERSION_OVERRIDE})
  else()
    message(FATAL_ERROR "Invalid version supplied - must be <MAJOR>.<MINOR>.<PATCH>[-(rc|beta)<NUMBER>].")
  endif()
endif()

# Set beta/rc versions if suffix included in version string
if(_plugin_version MATCHES "v?[0-9]+\\.[0-9]+\\.[0-9]+-rc[0-9]+")
  string(REGEX REPLACE "v?[0-9]+\\.[0-9]+\\.[0-9]+-rc([0-9]+).*$" "\\1" _plugin_release_candidate ${_plugin_version})
elseif(_plugin_version MATCHES "v?[0-9]+\\.[0-9]+\\.[0-9]+-beta[0-9]+")
  string(REGEX REPLACE "v?[0-9]+\\.[0-9]+\\.[0-9]+-beta([0-9]+).*$" "\\1" _plugin_beta ${_plugin_version})
endif()

list(GET _plugin_version_canonical 0 PLUGIN_VERSION_MAJOR)
list(GET _plugin_version_canonical 1 PLUGIN_VERSION_MINOR)
list(GET _plugin_version_canonical 2 PLUGIN_VERSION_PATCH)

set(PLUGIN_RELEASE_CANDIDATE ${_plugin_release_candidate})
set(PLUGIN_BETA ${_plugin_beta})

string(REPLACE ";" "." PLUGIN_VERSION_CANONICAL "${_plugin_version_canonical}")
string(REPLACE ";" "." PLUGIN_VERSION "${_plugin_version}")

if(PLUGIN_RELEASE_CANDIDATE GREATER 0)
  message(
    AUTHOR_WARNING
    "******************************************************************************\n"
    "  + ${_name} - Release candidate detected, PLUGIN_VERSION is now: ${PLUGIN_VERSION}\n"
    "******************************************************************************"
  )
elseif(PLUGIN_BETA GREATER 0)
  message(
    AUTHOR_WARNING
    "******************************************************************************\n"
    "  + ${_name} - Beta detected, PLUGIN_VERSION is now: ${PLUGIN_VERSION}\n"
    "******************************************************************************"
  )
endif()

unset(_plugin_default_version)
unset(_plugin_version)
unset(_plugin_version_canonical)
unset(_plugin_release_candidate)
unset(_plugin_beta)
unset(_plugin_version_result)
