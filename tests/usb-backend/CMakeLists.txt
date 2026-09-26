# Unit tests for the USB camera backend logic in src/ptz-usb-backend.hpp and
# src/ptz-usb-backend-slot.hpp, and for the worker thread in
# src/ptz-usb-worker.*. Needs no camera or OBS: the tests use a fake backend.
# The worker is a QObject with a thread, so this does need Qt Core.

# macOS's cmake/macos/xcode.cmake sets CMAKE_SKIP_RPATH TRUE project-wide, which
# makes the BUILD_RPATH below a no-op. This is a plain executable, so re-enable
# it, scoped to this subdirectory (same as tests/uart-hil). No-op elsewhere.
set(CMAKE_SKIP_RPATH FALSE)

add_executable(
  usb-backend-tests
  main.cpp
  test_backend.cpp
  test_uvc_protocol.cpp
  test_slot.cpp
  test_worker.cpp
  "${CMAKE_SOURCE_DIR}/src/ptz-usb-worker.hpp"
  "${CMAKE_SOURCE_DIR}/src/ptz-usb-worker.cpp"
)

target_include_directories(usb-backend-tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(usb-backend-tests PRIVATE Qt6::Core catch2_amalgamated)
set_target_properties(usb-backend-tests PROPERTIES AUTOMOC ON CXX_STANDARD 17)

# Point RPATH at the Qt libraries the plugin itself builds against (see
# tests/uart-hil/CMakeLists.txt)
if(NOT WIN32)
  set(_usb_tests_rpaths "")
  foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
    list(APPEND _usb_tests_rpaths "${_prefix}/Frameworks" "${_prefix}/lib")
  endforeach()
  set_target_properties(
    usb-backend-tests
    PROPERTIES BUILD_RPATH "${_usb_tests_rpaths}" INSTALL_RPATH "${_usb_tests_rpaths}"
  )
endif()

# The Xcode generator signs targets ad-hoc with the hardened runtime, whose
# library validation then refuses to load the (differently signed) Qt frameworks
# into a plain test executable. The plugin gets away with it through the
# entitlements OBS is signed with; a test executable isn't shipped, so just
# don't use the hardened runtime.
if(APPLE)
  set_target_properties(usb-backend-tests PROPERTIES XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME NO)
endif()
