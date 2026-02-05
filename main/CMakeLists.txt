# Generate version string from git
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

idf_component_register(
    SRCS
        "main.c"
        "display.c"
        "touch.c"
        "wifi.c"
        "nvs_config.c"
        "ui_clock.c"
        "ui_wifi_setup.c"
        "ui_timezone.c"
        "ui_settings.c"
        "ui_about.c"
        "ui_ntp.c"
    INCLUDE_DIRS "."
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE VERSION_STRING="${VERSION_STRING}")
