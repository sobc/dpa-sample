cmake_minimum_required(VERSION 3.20)
project(dpa_mirror LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Setup CMake module path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Include CPM.cmake package manager
include(cmake/CPM.cmake)

# Fetch CLI11 for command-line parsing
CPMAddPackage(
    NAME CLI11
    GITHUB_REPOSITORY CLIUtils/CLI11
    VERSION 2.4.2
    OPTIONS "CLI11_BUILD_TESTS OFF" "CLI11_BUILD_EXAMPLES OFF"
)

# Find system libraries via PkgConfig
find_package(PkgConfig REQUIRED)
pkg_check_modules(FLEXIO REQUIRED libflexio)
pkg_check_modules(IBVERBS REQUIRED libibverbs)
pkg_check_modules(MLX5 REQUIRED libmlx5)

# Find NVIDIA DPACC compiler
find_package(DPACC REQUIRED)

# Compile DPA device code into a statically linked archive containing the DPA binary
dpa_add_application(dpa_mirror_dev
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/device/dpa_mirror_dev.c"
    APP_NAME "dpa_mirror_app"
    MCPU "nv-dpa-bf3"
    INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# Host application executable
add_executable(dpa_mirror
    src/host/main.cpp
    src/host/flow_steering.cpp
)

target_include_directories(dpa_mirror PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/host"
    ${FLEXIO_INCLUDE_DIRS}
    ${IBVERBS_INCLUDE_DIRS}
    ${MLX5_INCLUDE_DIRS}
)

target_compile_definitions(dpa_mirror PRIVATE
    FLEXIO_VER_USED=FLEXIO_VER_LATEST
    FLEXIO_ALLOW_EXPERIMENTAL_API
)

target_compile_options(dpa_mirror PRIVATE
    -Wall -Wextra -Wpedantic
    ${FLEXIO_CFLAGS_OTHER}
    ${IBVERBS_CFLAGS_OTHER}
    ${MLX5_CFLAGS_OTHER}
)

target_link_directories(dpa_mirror PRIVATE
    ${FLEXIO_LIBRARY_DIRS}
    ${IBVERBS_LIBRARY_DIRS}
    ${MLX5_LIBRARY_DIRS}
)

# Link DPA device archive with --whole-archive so the embedded DPA program descriptors are preserved
target_link_libraries(dpa_mirror PRIVATE
    -Wl,--whole-archive dpa_mirror_dev -Wl,--no-whole-archive
    CLI11::CLI11
    ${FLEXIO_LIBRARIES}
    ${IBVERBS_LIBRARIES}
    ${MLX5_LIBRARIES}
    pthread
)
