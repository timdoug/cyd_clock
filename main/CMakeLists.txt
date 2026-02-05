# Version header generated at build time
set(VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/version.h")

idf_component_register(
    SRCS
        "main.c"
        "display.c"
        "led.c"
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

# Generate version.h at build time (always runs)
add_custom_target(version_header ALL
    COMMAND ${CMAKE_COMMAND} -DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/version.cmake
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    BYPRODUCTS ${VERSION_HEADER}
    COMMENT "Generating version.h"
)

# Make sure main depends on version header
add_dependencies(${COMPONENT_LIB} version_header)
