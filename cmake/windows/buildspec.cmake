# CMake Windows build dependencies module

include_guard(GLOBAL)

include(buildspec_common)

# _check_dependencies_windows: Set up Windows slice for _check_dependencies
function(_check_dependencies_windows)
  # CMAKE_VS_PLATFORM_NAME preserves whatever case was passed to -A (e.g.
  # "ARM64"), but the obs-deps release asset names and buildspec.json hash
  # keys are lowercase, so normalize before using it for lookups/filenames.
  string(TOLOWER "${CMAKE_VS_PLATFORM_NAME}" arch)
  set(platform windows-${arch})

  set(dependencies_dir "${CMAKE_CURRENT_SOURCE_DIR}/.deps")
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
  set(dependencies_list prebuilt qt6 qtserialport sdl obs-studio)

  _check_dependencies()
endfunction()

_check_dependencies_windows()
