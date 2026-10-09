# CMake Linux defaults module

include_guard(GLOBAL)

# Set default installation directories
include(GNUInstallDirs)

if(CMAKE_INSTALL_LIBDIR MATCHES "(CMAKE_SYSTEM_PROCESSOR)")
  string(REPLACE "CMAKE_SYSTEM_PROCESSOR" "${CMAKE_SYSTEM_PROCESSOR}" CMAKE_INSTALL_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
endif()

# Enable find_package targets to become globally available targets
set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)

set(CPACK_PACKAGE_NAME "${CMAKE_PROJECT_NAME}")
set(CPACK_PACKAGE_VERSION "${CMAKE_PROJECT_VERSION}")

# Use the Debian Policy-compliant name_version_arch.deb naming (DEB-DEFAULT).
# The Debian version is derived from the full version string (which may carry
# a pre-release suffix, e.g. "v0.20.0-pre2" or "0.20.0-pre2-6-gdf53732a"), with
# "~" introducing the pre-release so it sorts before the final release, and any
# commits-since-tag folded into a "+N.gHASH" suffix (a plain "-" would be
# parsed as the Debian revision separator). The distro codename is appended
# as a further "~" suffix (the convention Ubuntu PPAs use), when known
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
string(REGEX REPLACE "^v" "" _deb_version "${_version}")
if(
  _deb_version
    MATCHES
    "^([0-9]+\\.[0-9]+\\.[0-9]+)(-([A-Za-z][A-Za-z0-9.]*))?(-([0-9]+)-(g[0-9a-f]+)(-dirty)?)?$"
)
  set(_deb_version "${CMAKE_MATCH_1}")
  if(CMAKE_MATCH_3)
    string(APPEND _deb_version "~${CMAKE_MATCH_3}")
  endif()
  if(CMAKE_MATCH_5)
    string(APPEND _deb_version "+${CMAKE_MATCH_5}.${CMAKE_MATCH_6}")
  endif()
else()
  string(REPLACE "-" "~" _deb_version "${_deb_version}")
endif()
set(CPACK_DEBIAN_PACKAGE_VERSION "${_deb_version}")
if(EXISTS "/etc/os-release")
  file(STRINGS "/etc/os-release" _os_release_codename REGEX "^VERSION_CODENAME=")
  if(_os_release_codename)
    string(REGEX REPLACE "^VERSION_CODENAME=\"?([^\"]*)\"?$" "\\1" _os_codename "${_os_release_codename}")
    string(APPEND CPACK_DEBIAN_PACKAGE_VERSION "~${_os_codename}")
  endif()
endif()

set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_C_LIBRARY_ARCHITECTURE}")

set(CPACK_GENERATOR "DEB")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${PLUGIN_EMAIL}")
set(CPACK_SET_DESTDIR ON)

if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.25.0 OR NOT CMAKE_CROSSCOMPILING)
  set(CPACK_DEBIAN_DEBUGINFO_PACKAGE ON)
endif()

set(CPACK_OUTPUT_FILE_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/release")

set(CPACK_SOURCE_GENERATOR "TXZ")
set(
  CPACK_SOURCE_IGNORE_FILES
  ".*~$"
  \\.git/
  \\.github/
  \\.gitignore
  \\.ccache/
  build_.*
  cmake/\\.CMakeBuildNumber
  release/
)

set(CPACK_VERBATIM_VARIABLES YES)
# Name the source tarball after the full version (including any pre-release
# suffix), without the leading "v" of a git tag
string(REGEX REPLACE "^v" "" _source_version "${_version}")
set(CPACK_SOURCE_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${_source_version}-source")
set(CPACK_ARCHIVE_THREADS 0)

include(CPack)

find_package(libobs QUIET)

if(NOT TARGET OBS::libobs)
  find_package(LibObs REQUIRED)
  add_library(OBS::libobs ALIAS libobs)

  if(ENABLE_FRONTEND_API)
    find_path(
      obs-frontend-api_INCLUDE_DIR
      NAMES obs-frontend-api.h
      PATHS /usr/include /usr/local/include
      PATH_SUFFIXES obs
    )

    find_library(obs-frontend-api_LIBRARY NAMES obs-frontend-api PATHS /usr/lib /usr/local/lib)

    if(obs-frontend-api_LIBRARY)
      if(NOT TARGET OBS::obs-frontend-api)
        if(IS_ABSOLUTE "${obs-frontend-api_LIBRARY}")
          add_library(OBS::obs-frontend-api UNKNOWN IMPORTED)
          set_property(TARGET OBS::obs-frontend-api PROPERTY IMPORTED_LOCATION "${obs-frontend-api_LIBRARY}")
        else()
          add_library(OBS::obs-frontend-api INTERFACE IMPORTED)
          set_property(TARGET OBS::obs-frontend-api PROPERTY IMPORTED_LIBNAME "${obs-frontend-api_LIBRARY}")
        endif()

        set_target_properties(
          OBS::obs-frontend-api
          PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${obs-frontend-api_INCLUDE_DIR}"
        )
      endif()
    endif()
  endif()

  macro(find_package)
    if(NOT "${ARGV0}" STREQUAL libobs AND NOT "${ARGV0}" STREQUAL obs-frontend-api)
      _find_package(${ARGV})
    endif()
  endmacro()
endif()
