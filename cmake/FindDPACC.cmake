# FindDPACC.cmake - CMake Module for NVIDIA DPACC (Data Path Accelerator Compiler)

find_program(DPACC_EXECUTABLE
    NAMES dpacc
    PATHS
        /opt/mellanox/doca/tools
        /usr/bin
        /usr/local/bin
        ENV DOCA_DIR
    PATH_SUFFIXES tools
    DOC "Path to DPACC compiler"
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(DPACC
    REQUIRED_VARS DPACC_EXECUTABLE
)

if(DPACC_FOUND)
    execute_process(
        COMMAND ${DPACC_EXECUTABLE} --version
        OUTPUT_VARIABLE DPACC_VERSION_OUTPUT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(DPACC_VERSION_OUTPUT MATCHES "dpacc version ([0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+)")
        set(DPACC_VERSION "${CMAKE_MATCH_1}")
    endif()
    message(STATUS "Found DPACC: ${DPACC_EXECUTABLE} (version ${DPACC_VERSION})")
endif()

# Function to add a DPA application archive
function(dpa_add_application TARGET_NAME)
    cmake_parse_arguments(DPA "" "APP_NAME;MCPU" "SOURCES;INCLUDES;DEVICE_OPTIONS;HOST_OPTIONS" ${ARGN})

    if(NOT DPA_APP_NAME)
        set(DPA_APP_NAME "${TARGET_NAME}")
    endif()

    if(NOT DPA_MCPU)
        set(DPA_MCPU "nv-dpa-bf3")
    endif()

    set(OUTPUT_ARCHIVE "${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.a")

    # Common device compiler options
    set(DEV_CC_OPTS
        "-DE_MODE_LE"
        "-ffreestanding"
        "-mcmodel=medany"
        "-DFLEXIO_DEV_ALLOW_EXPERIMENTAL_API"
        "-DFLEXIO_DEV_VER_USED=FLEXIO_DEV_VER_LATEST"
        "-Wall"
        "-Wextra"
    )

    # Append user device options
    if(DPA_DEVICE_OPTIONS)
        list(APPEND DEV_CC_OPTS ${DPA_DEVICE_OPTIONS})
    endif()

    # Optimization based on build type
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        list(APPEND DEV_CC_OPTS "-g" "-O0" "-gdwarf-4")
        set(EXTRA_DPACC_OPTS "")
    else()
        list(APPEND DEV_CC_OPTS "-O3")
        set(EXTRA_DPACC_OPTS "--flto")
    endif()

    # Includes
    foreach(INC ${DPA_INCLUDES} /opt/mellanox/flexio/include)
        list(APPEND DEV_CC_OPTS "-I${INC}")
    endforeach()

    # Host compiler options
    set(HOST_CC_OPTS "-fPIC,-DFLEXIO_ALLOW_EXPERIMENTAL_API")
    if(DPA_HOST_OPTIONS)
        string(JOIN "," HOST_EXTRA ${DPA_HOST_OPTIONS})
        set(HOST_CC_OPTS "${HOST_CC_OPTS},${HOST_EXTRA}")
    endif()

    # Join device options with commas as expected by dpacc
    string(JOIN "," DEV_CC_OPTS_JOINED ${DEV_CC_OPTS})

    add_custom_command(
        OUTPUT "${OUTPUT_ARCHIVE}"
        COMMAND ${DPACC_EXECUTABLE}
            ${DPA_SOURCES}
            -o "${OUTPUT_ARCHIVE}"
            -hostcc=${CMAKE_C_COMPILER}
            "--devicecc-options=${DEV_CC_OPTS_JOINED}"
            "-hostcc-options=${HOST_CC_OPTS}"
            "--mcpu=${DPA_MCPU}"
            ${EXTRA_DPACC_OPTS}
            "--app-name=${DPA_APP_NAME}"
        DEPENDS ${DPA_SOURCES}
        COMMENT "Compiling DPA application ${DPA_APP_NAME} for ${DPA_MCPU}"
        VERBATIM
    )

    add_custom_target(${TARGET_NAME}_target ALL DEPENDS "${OUTPUT_ARCHIVE}")

    add_library(${TARGET_NAME} STATIC IMPORTED GLOBAL)
    set_target_properties(${TARGET_NAME} PROPERTIES
        IMPORTED_LOCATION "${OUTPUT_ARCHIVE}"
    )
    add_dependencies(${TARGET_NAME} ${TARGET_NAME}_target)
endfunction()
