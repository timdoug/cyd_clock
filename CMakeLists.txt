cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Board profile (see main/boards/); main/CMakeLists.txt validates the value.
# The board is part of the project name so it lands in the app descriptor's
# project_name field, letting the OTA identity check refuse the other
# board's image. Release asset names are unaffected (the CI workflow renames
# the binaries when uploading).
if(NOT DEFINED CYD_BOARD OR CYD_BOARD STREQUAL "")
    set(CYD_BOARD "esp32_2432s028")
endif()
project(cyd_clock-${CYD_BOARD})
