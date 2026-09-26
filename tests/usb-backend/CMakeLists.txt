# Unit tests for the USB camera backend logic in src/ptz-usb-backend.hpp and
# src/ptz-usb-backend-slot.hpp. Needs no camera, OBS or Qt: those headers are
# self-contained, and the tests use a fake backend.

add_executable(usb-backend-tests main.cpp test_backend.cpp test_uvc_protocol.cpp test_slot.cpp)

target_include_directories(usb-backend-tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(usb-backend-tests PRIVATE catch2_amalgamated)
set_target_properties(usb-backend-tests PROPERTIES CXX_STANDARD 17)
