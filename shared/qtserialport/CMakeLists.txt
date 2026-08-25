# Vendored + reworked qtserialport (see README.md and the two commits
# that brought this directory in: the vendor commit and the rework
# commit on top of it). Entered on APPLE and WIN32; Linux never enters
# this directory at all (keeps linking the real, distro-packaged
# Qt6::SerialPort, which never had the ABI problem this fork solves).

if(APPLE OR WIN32)
  add_library(
    qtserialport STATIC
    src/qserialport.h
    src/qserialport.cpp
    src/qserialport_p.h
    src/qserialportglobal.h
    src/qserialportinfo.h
    src/qserialportinfo.cpp
    src/qserialportinfo_p.h
    src/qtserialport_ringbuffer_compat_p.h
  )

  target_include_directories(qtserialport PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

  set_target_properties(qtserialport PROPERTIES CXX_STANDARD 17 POSITION_INDEPENDENT_CODE ON AUTOMOC ON
                                                 COMPILE_WARNING_AS_ERROR OFF)

  target_link_libraries(qtserialport PUBLIC Qt6::Core)

  if(APPLE)
    target_sources(
      qtserialport PRIVATE
      src/qserialport_unix.cpp
      src/qserialportinfo_osx.cpp
      src/qtserialport_unix_compat_p.h
      src/qtserialport_mac_compat_p.h
    )

    # Vendored upstream code (see README.md) - e.g. qserialport_unix.cpp's
    # calls into qt_subtract_from_timeout(int, int), truncating
    # QElapsedTimer::elapsed()'s qint64 return at the call site, unchanged
    # from real Qt. Harmless in practice (these are all sub-second
    # timeouts), and not something to fix by editing vendored call sites
    # just to satisfy a warning flag Qt's own build doesn't enable here.
    target_compile_options(qtserialport PRIVATE -Wno-shorten-64-to-32)

    find_library(IOKIT_FRAMEWORK IOKit REQUIRED)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    target_link_libraries(qtserialport PRIVATE ${IOKIT_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})
  elseif(WIN32)
    target_sources(
      qtserialport PRIVATE
      src/qserialport_win.cpp
      src/qserialportinfo_win.cpp
      src/qwinoverlappedionotifier_p.h
      src/qwinoverlappedionotifier.cpp
      src/qtserialport_win_compat_p.h
    )

    # Same as upstream's own qt_internal_extend_target(SerialPort CONDITION
    # WIN32 ... LIBRARIES advapi32 setupapi) - registry access
    # (qserialportinfo_win.cpp's QWinRegistryKey stand-in) and device
    # enumeration (SetupDiXxx calls).
    target_link_libraries(qtserialport PRIVATE advapi32 setupapi)
  endif()
endif()
