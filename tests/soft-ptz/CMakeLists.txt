# Unit tests for the soft PTZ viewport model in src/ptz-soft-viewport.hpp.
# A header-only class with no Qt or OBS dependency, so needs neither.

# macOS's cmake/macos/xcode.cmake sets CMAKE_SKIP_RPATH TRUE project-wide, which
# would make any RPATH setting a no-op. Re-enable it, scoped to this
# subdirectory (same as tests/usb-backend). No-op elsewhere.
set(CMAKE_SKIP_RPATH FALSE)

add_executable(
  soft-ptz-tests
  main.cpp
  test_viewport.cpp
  test_motion.cpp
  "${CMAKE_CURRENT_LIST_DIR}/../../src/ptz-soft-viewport.hpp"
)

target_include_directories(soft-ptz-tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src")
target_link_libraries(soft-ptz-tests PRIVATE catch2_amalgamated)
set_target_properties(soft-ptz-tests PROPERTIES CXX_STANDARD 17)

# The Xcode generator signs targets ad-hoc with the hardened runtime, which a
# plain test executable isn't shipped with (see tests/usb-backend/CMakeLists.txt)
if(APPLE)
  set_target_properties(soft-ptz-tests PROPERTIES XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME NO)
endif()
