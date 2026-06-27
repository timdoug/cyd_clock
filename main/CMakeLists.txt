set(VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/version.h")

idf_component_register(
    SRCS
        "main.c"
        "display.c"
        "led.c"
        "touch.c"
        "wifi.c"
        "ntp.c"
        "ntp_benchmark.c"
        "ntp_time.c"
        "ntp_peers.c"
        "ntp_io.c"
        "ntp_wifi_stamp.c"
        "ntp_filter.c"
        "ntp_discipline.c"
        "ntp_siv.c"
        "ntp_nts.c"
        "ota_update.c"
        "nvs_config.c"
        "i18n.c"
        "ui_common.c"
        "ui_language.c"
        "ui_keyboard.c"
        "ui_clock.c"
        "ui_wifi_setup.c"
        "ui_timezone.c"
        "ui_settings.c"
        "ui_about.c"
        "ui_ntp.c"
        "ui_ntp_stats.c"
    INCLUDE_DIRS
        "."
        "${CMAKE_CURRENT_BINARY_DIR}"
    PRIV_REQUIRES
        app_update
        bootloader_support
        esp_event
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_spi
        esp_https_ota
        esp_http_client
        esp_hw_support
        esp-tls
        mbedtls
        esp_netif
        esp_system
        esp_timer
        esp_wifi
        lwip
        nvs_flash
)

add_custom_target(version_header ALL
    COMMAND
        ${CMAKE_COMMAND}
        -DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/version.cmake
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    BYPRODUCTS ${VERSION_HEADER}
    COMMENT "Generating version.h"
    VERBATIM
)

add_dependencies(${COMPONENT_LIB} version_header)
