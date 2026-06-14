if(NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "OUTPUT_DIR is required")
endif()

execute_process(
    COMMAND git log -1 --format=%cs
    OUTPUT_VARIABLE GIT_DATE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

execute_process(
    COMMAND git rev-parse --short HEAD
    OUTPUT_VARIABLE GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

execute_process(
    COMMAND git diff-index --quiet HEAD --
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

set(VERSION_HEADER "${OUTPUT_DIR}/version.h")
set(VERSION_CONTENT "#define VERSION_STRING \"${VERSION_STRING}\"\n")

set(OLD_CONTENT "")
if(EXISTS "${VERSION_HEADER}")
    file(READ "${VERSION_HEADER}" OLD_CONTENT)
endif()

if(NOT "${OLD_CONTENT}" STREQUAL "${VERSION_CONTENT}")
    file(WRITE "${VERSION_HEADER}" "${VERSION_CONTENT}")
endif()
