# Vendored catchorg/Catch2 (Boost Software License 1.0), amalgamated
# two-file distribution. See README.md in this directory for provenance.
# Only used by the tests (tests/uart-hil/, tests/usb-backend/), never by the obs-ptz plugin itself.

add_library(catch2_amalgamated STATIC catch_amalgamated.cpp catch_amalgamated.hpp)

target_include_directories(catch2_amalgamated PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# catch_amalgamated.cpp defines its own main() unless told not to
# (CATCH_AMALGAMATED_CUSTOM_MAIN, documented right above that definition in
# the file) - each test target supplies its own, so without this
# the link fails with "multiple definition of `main`".
target_compile_definitions(catch2_amalgamated PUBLIC CATCH_AMALGAMATED_CUSTOM_MAIN)

set_target_properties(catch2_amalgamated PROPERTIES CXX_STANDARD 17 POSITION_INDEPENDENT_CODE ON)

# Vendored, unmodified upstream source - not held to this project's own
# warnings-as-errors policy (CMAKE_COMPILE_WARNING_AS_ERROR, true in every
# CMakePresets.json preset). Same reasoning and same generator-specific
# belt-and-braces MSVC override as shared/serial/CMakeLists.txt.
set_target_properties(catch2_amalgamated PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
if(MSVC)
  target_compile_options(catch2_amalgamated PRIVATE /WX-)
  target_link_options(catch2_amalgamated PRIVATE /WX-)
endif()
