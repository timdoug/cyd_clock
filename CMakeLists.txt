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

# App-descriptor version: use the same git-derived string version.h carries,
# so the OTA same-version gate and the About screen agree. Without an
# explicit PROJECT_VER, IDF captures `git describe` at configure time and an
# incremental build after new commits ships a stale descriptor that makes
# devices refuse the genuinely newer image. The CONFIGURE_DEPENDS entries
# force a reconfigure when HEAD moves (commit/checkout) or the index changes.
include(${CMAKE_CURRENT_LIST_DIR}/main/version.cmake)
set(PROJECT_VER ${VERSION_STRING})
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/.git/logs/HEAD)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_LIST_DIR}/.git/logs/HEAD
        ${CMAKE_CURRENT_LIST_DIR}/.git/index)
endif()

project(cyd_clock-${CYD_BOARD})
