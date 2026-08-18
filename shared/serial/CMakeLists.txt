# Vendored gbionics/serial_cpp (MIT). See README.md in this directory for
# provenance. Upstream ships its own modern, install/FetchContent-friendly
# CMakeLists.txt, but this is a small first-party one instead, matching
# this project's other vendored-dependency directories: only the sources
# this build actually needs, none of upstream's install/package-config/
# examples/tests machinery.
#
# Unlike shared/libserialport, this is vendored on *all* platforms,
# including Linux - there's no distro package for this library the way
# there is for libserialport (see README.md).

add_library(serial_cpp STATIC src/serial.cc include/serial_cpp/serial.h include/serial_cpp/v8stdint.h)

target_include_directories(serial_cpp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

set_target_properties(serial_cpp PROPERTIES CXX_STANDARD 17 POSITION_INDEPENDENT_CODE ON)

# This is vendored upstream source (see README.md) that we don't modify, so
# it isn't held to this project's own warnings-as-errors policy, which
# CMake would otherwise apply here too since this is a subdirectory of the
# same build. This project drives that policy through CMake's standard
# CMAKE_COMPILE_WARNING_AS_ERROR (true in every CMakePresets.json preset;
# see cmake/common/compiler_common.cmake), which sets the COMPILE_WARNING_AS_ERROR
# target property at the point add_library() runs above - override it back
# off for just this target, the correct generator-agnostic way to opt out.
set_target_properties(serial_cpp PROPERTIES COMPILE_WARNING_AS_ERROR OFF)

# Belt-and-braces alongside the property above, and the only thing that
# actually works under the Xcode generator specifically (see the
# shared/libserialport/CMakeLists.txt comment on the same issue for the
# full explanation - that project doesn't fully wire COMPILE_WARNING_AS_ERROR
# through to the Xcode generator's actual compile command line, only
# per-diagnostic -Wno-error=<name> flags reliably suppress it there).
# -Wshorten-64-to-32: unix.cc narrows a few long/int64_t timeout
# calculations to uint32_t (Timeout's fields are uint32_t) - intentional
# on upstream's part, just not clean under this project's stricter flags.
# -Wdeprecated-declarations: list_ports_osx.cc calls IOMasterPort(), which
# Apple deprecated in macOS 12 (in favor of kIOMainPortDefault) - not yet
# fixed upstream; still works today, just triggers a warning on modern SDKs.
#
# These two diagnostic names are Clang-specific (AppleClang on macOS, or
# plain Clang if used on Linux/Windows instead of GCC/MSVC) - GCC doesn't
# have them at all and, unlike an unrecognized plain -W<name>, treats an
# unrecognized -Wno-error=<name> as a hard error, so this must not apply
# unconditionally to "not MSVC". GCC needs no equivalent override for
# these two specifically: on Linux it already builds with -Wno-conversion
# -Wno-float-conversion -Wno-deprecated-declarations globally
# (cmake/linux/compilerconfig.cmake), so neither fires there.
#
# GCC does still need its own, differently-named overrides below though -
# confirmed by compiling this target standalone under the same flag set
# cmake/linux/compilerconfig.cmake adds project-wide for GNU (Ninja
# generator, so this isn't the Xcode-generator quirk above - GCC just
# enables a few more warnings this vendored code doesn't happen to be
# clean under):
# -Wsign-compare (part of -Wextra, which this project always enables) fires
#  on a couple of int/size_t comparisons in list_ports_linux.cc.
# -Wunused-but-set-variable fires on a glob() return value upstream checks
#  the sign of but never otherwise reads, also in list_ports_linux.cc.
#
# Note: unlike upstream wjwwood/serial, this fork already fixes the
# double/stopbits_t arithmetic warning (-Wenum-float-conversion on Clang,
# -Wenum-conversion on GCC) with an explicit static_cast, so neither of
# those needs suppressing here.
if(MSVC)
  target_compile_options(serial_cpp PRIVATE /WX-)
  target_link_options(serial_cpp PRIVATE /WX-)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(serial_cpp PRIVATE -Wno-error=shorten-64-to-32 -Wno-error=deprecated-declarations)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(serial_cpp PRIVATE -Wno-error=sign-compare -Wno-error=unused-but-set-variable)
endif()

if(WIN32)
  target_sources(serial_cpp PRIVATE src/impl/win.cc include/serial_cpp/impl/win.h src/impl/list_ports/list_ports_win.cc)
  target_link_libraries(serial_cpp PRIVATE setupapi)
else()
  target_sources(serial_cpp PRIVATE src/impl/unix.cc include/serial_cpp/impl/unix.h)
  find_package(Threads REQUIRED)
  target_link_libraries(serial_cpp PRIVATE Threads::Threads)
  if(APPLE)
    target_sources(serial_cpp PRIVATE src/impl/list_ports/list_ports_osx.cc)
    find_library(IOKIT_FRAMEWORK IOKit REQUIRED)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    target_link_libraries(serial_cpp PRIVATE ${IOKIT_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})
  else()
    target_sources(serial_cpp PRIVATE src/impl/list_ports/list_ports_linux.cc)
  endif()
endif()
