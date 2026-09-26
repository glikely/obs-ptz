# usb-uvc-position: reads a USB camera's real position through the macOS
# backend, for tests/usb-e2e/. See uvc-position.cpp.

add_executable(usb-uvc-position uvc-position.cpp "${CMAKE_SOURCE_DIR}/src/ptz-usb-backend-macos.cpp")

target_include_directories(usb-uvc-position PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(usb-uvc-position PRIVATE OBS::libobs "-framework IOKit" "-framework CoreFoundation")
set_target_properties(usb-uvc-position PROPERTIES CXX_STANDARD 17 XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME NO)

# Same as tests/usb-backend: libobs and its dependencies are found through
# RPATH, and this is a plain executable that isn't shipped
set(CMAKE_SKIP_RPATH FALSE)
set(_usb_e2e_rpaths "")
foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
  list(APPEND _usb_e2e_rpaths "${_prefix}/Frameworks" "${_prefix}/lib")
endforeach()
set_target_properties(usb-uvc-position PROPERTIES BUILD_RPATH "${_usb_e2e_rpaths}" INSTALL_RPATH "${_usb_e2e_rpaths}")
