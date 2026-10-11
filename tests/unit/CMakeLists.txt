# Unit tests for code that needs neither OBS nor Qt (src/core/ptz-lifecycle.hpp).
#
# Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
# SPDX-License-Identifier: GPL-2.0-or-later

# macOS's cmake/macos/xcode.cmake sets CMAKE_SKIP_RPATH TRUE project-wide, which
# would make any RPATH setting a no-op. Re-enable it, scoped to this
# subdirectory (same as tests/soft-ptz). No-op elsewhere.
set(CMAKE_SKIP_RPATH FALSE)

add_executable(ptz-unit-tests main.cpp test_lifecycle.cpp "${CMAKE_CURRENT_LIST_DIR}/../../src/core/ptz-lifecycle.hpp")

target_include_directories(ptz-unit-tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/core")
target_link_libraries(ptz-unit-tests PRIVATE catch2_amalgamated)
set_target_properties(ptz-unit-tests PROPERTIES CXX_STANDARD 17)

# The Xcode generator signs targets ad-hoc with the hardened runtime, which a
# plain test executable isn't shipped with (see tests/soft-ptz/CMakeLists.txt)
if(APPLE)
  set_target_properties(ptz-unit-tests PROPERTIES XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME NO)
endif()

add_test(NAME lifecycle COMMAND ptz-unit-tests "[lifecycle]")
