# Generate version string from git (runs at configure time)
execute_process(
    COMMAND git log -1 --format=%cs
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_DATE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

execute_process(
    COMMAND git diff-index --quiet HEAD --
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    RESULT_VARIABLE GIT_DIRTY
)

if(GIT_DATE AND GIT_HASH)
    set(VERSION_STRING "${GIT_DATE}_${GIT_HASH}")
    if(GIT_DIRTY)
        set(VERSION_STRING "${VERSION_STRING}+dirty")
    endif()
else()
    set(VERSION_STRING "unknown")
endif()

# Write version.h
set(VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/version.h")
file(WRITE ${VERSION_HEADER} "#define VERSION_STRING \"${VERSION_STRING}\"\n")

idf_component_register(
    SRCS
        "main.c"
        "display.c"
        "touch.c"
        "wifi.c"
        "nvs_config.c"
        "ui_common.c"
        "ui_clock.c"
        "ui_wifi_setup.c"
        "ui_timezone.c"
        "ui_settings.c"
        "ui_about.c"
        "ui_ntp.c"
    INCLUDE_DIRS "." "${CMAKE_CURRENT_BINARY_DIR}"
)

# Force CMake to reconfigure when git index changes
# This ensures version string stays up to date
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../.git/index"
)
