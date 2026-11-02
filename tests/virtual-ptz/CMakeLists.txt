# Unit tests for the virtual PTZ viewport model in src/drivers/virtual/ptz-virtual-viewport.hpp.
# A header-only class with no Qt or OBS dependency, so needs neither.

# macOS's cmake/macos/xcode.cmake sets CMAKE_SKIP_RPATH TRUE project-wide, which
# would make any RPATH setting a no-op. Re-enable it, scoped to this
# subdirectory (same as tests/usb-backend). No-op elsewhere.
set(CMAKE_SKIP_RPATH FALSE)

add_executable(
  virtual-ptz-tests
  main.cpp
  test_viewport.cpp
  test_motion.cpp
  "${CMAKE_CURRENT_LIST_DIR}/../../src/drivers/virtual/ptz-virtual-viewport.hpp"
)

target_include_directories(virtual-ptz-tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/drivers/virtual")
target_link_libraries(virtual-ptz-tests PRIVATE catch2_amalgamated)
set_target_properties(virtual-ptz-tests PROPERTIES CXX_STANDARD 17)

# The Xcode generator signs targets ad-hoc with the hardened runtime, which a
# plain test executable isn't shipped with (see tests/usb-backend/CMakeLists.txt)
if(APPLE)
  set_target_properties(virtual-ptz-tests PROPERTIES XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME NO)
endif()
