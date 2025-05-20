# CMake Windows build dependencies module

include_guard(GLOBAL)

include(buildspec_common)

# _check_dependencies_windows: Set up Windows slice for _check_dependencies
function(_check_dependencies_windows)
  set(arch ${CMAKE_VS_PLATFORM_NAME})
  set(platform windows-${arch})

  if(CMAKE_VS_PLATFORM_NAME MATCHES "(ARM64|arm64)" AND NOT QT_HOST_PATH)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/buildspec.json" buildspec)

    string(JSON dependency_data GET ${buildspec} dependencies)
    string(JSON data GET ${dependency_data} qt6)
    string(JSON version GET ${data} version)
    set(qt_x64_dir "${CMAKE_CURRENT_SOURCE_DIR}/.deps-x64/obs-deps-qt6-${version}-x64")

    if(IS_DIRECTORY "${qt_x64_dir}")
      set(QT_HOST_PATH "${qt_x64_dir}" CACHE STRING "Qt Host Tools Path" FORCE)
      set($ENV{QT_HOST_PATH} "${qt_x64_dir}")
    else()
      message(
        FATAL_ERROR
        "Building OBS Studio for Windows ARM64 requires x64 Qt dependencies, please run an x64 configure first (from an x64 native tools env) to ensure these are downloaded"
      )
    endif()
  endif()

  set(dependencies_dir "${CMAKE_CURRENT_SOURCE_DIR}/.deps-${arch}")
  set(prebuilt_filename "windows-deps-VERSION-ARCH-REVISION.zip")
  set(prebuilt_destination "obs-deps-VERSION-ARCH")
  set(qt6_filename "windows-deps-qt6-VERSION-ARCH-REVISION.zip")
  set(qt6_destination "obs-deps-qt6-VERSION-ARCH")
  set(qtserialport_filename "qtserialport-everywhere-src-VERSION.zip")
  set(qtserialport_destination "qtserialport-everywhere-src-VERSION")
  set(sdl_filename "SDL2-VERSION.zip")
  set(sdl_destination "SDL2-VERSION")
  set(obs-studio_filename "VERSION.zip")
  set(obs-studio_destination "obs-studio-VERSION")
  if(CMAKE_VS_PLATFORM_NAME MATCHES "(ARM64|arm64)")
    set(dependencies_list prebuilt qt6 sdl obs-studio)
  else()
    set(
      dependencies_list
      prebuilt
      qtserialport
      qt6
      sdl
      obs-studio
    )
  endif()

  _check_dependencies()
endfunction()

_check_dependencies_windows()
