# chronyd (third_party/chrony/src, vendored unmodified) built for ESP-IDF.
# port/ holds the platform code chrony's ./configure would otherwise pick:
# the system clock driver, the mbedTLS NTS backends, the entry point that
# replaces main.c, and POSIX shims (port/include) for headers newlib and
# lwIP lack.

set(CHRONY_SRC "${CMAKE_CURRENT_LIST_DIR}/../../third_party/chrony/src")

# chronyd's object list for this configuration (Makefile.in OBJS plus the
# NTS objects), less main.c and sys.c, which port/ replaces, and the NTS
# server objects, which port/nts_server_stubs.c replaces.
set(CHRONY_OBJS
    addrfilt array clientlog cmdparse conf keys local logging memory
    nameserv nameserv_async ntp_auth ntp_core ntp_ext ntp_io ntp_sources
    quantiles reference regress rtc samplefilt sched socket sources
    sourcestats stubs smooth sys_null tempcomp util hash_intmd5
    nts_ke_client nts_ke_session nts_ntp_auth nts_ntp_client)
list(TRANSFORM CHRONY_OBJS PREPEND "${CHRONY_SRC}/")
list(TRANSFORM CHRONY_OBJS APPEND ".c")

idf_component_register(
    SRCS
        ${CHRONY_OBJS}
        "port/chronyc_format.c"
        "port/chronyd_esp.c"
        "port/compat.c"
        "port/nts_server_stubs.c"
        "port/nvs_file.c"
        "port/siv_mbedtls.c"
        "port/sys.c"
        "port/sys_esp32.c"
        "port/tls_mbedtls.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "port/include"
        "${CHRONY_SRC}"
    PRIV_REQUIRES
        esp_netif
        lwip
        mbedtls
        nvs_flash
        pthread
        vfs
)

# Version from version.txt ("<tag>-<commits>-g<hash>", written by
# make update-chrony), as chrony's configure reads it.
file(STRINGS "${CHRONY_SRC}/version.txt" CHRONY_VERSION LIMIT_COUNT 1)
target_compile_definitions(${COMPONENT_LIB} PRIVATE CHRONY_VERSION="${CHRONY_VERSION}")

# chrony is not on a timing-critical path at the instruction level; size
# matters more in the OTA slot.
target_compile_options(${COMPONENT_LIB} PRIVATE -Os)

# Upstream sources are built as released: their warnings under IDF's
# -Wall -Wextra are reported but are not errors. port/ stays under -Werror.
set_source_files_properties(${CHRONY_OBJS} PROPERTIES COMPILE_OPTIONS
    "-Wno-error;-Wno-unused-parameter;-Wno-sign-compare;-Wno-missing-field-initializers;-Wno-empty-body")
