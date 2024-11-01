cmake_minimum_required(VERSION 3.5)

# Project name
project(ducktorrent)

# Add the libdht subdirectory to build it as part of the main project
add_subdirectory(libdht)

# Include directories for the project
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/libdht/include
    ${DUCKDB_INCLUDE_DIRS}  # Ensure this is set in your build environment
)

# Find OpenSSL package
find_package(OpenSSL REQUIRED)

# Set the extension name
set(EXTENSION_NAME ducktorrent_extension)

# Set sources for the extension
set(EXTENSION_SOURCES
    src/ducktorrent_extension.cpp
    ${LIB_SOURCES}
)

# Build the extension
add_library(${EXTENSION_NAME} SHARED ${EXTENSION_SOURCES})

# Set compile definitions
target_compile_definitions(${EXTENSION_NAME} PRIVATE
    DUCKDB_BUILD_LIBRARY
    DUCKDB_EXTENSION_LIBRARY
)

# Link dependencies, including libdht.a
target_link_libraries(${EXTENSION_NAME}
    PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
        dht  # Link the dht library
)

# Install the extension
install(TARGETS ${EXTENSION_NAME}
    EXPORT DuckDBExports
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}"
)

# Install export set
install(EXPORT DuckDBExports
    FILE DuckDBTargets.cmake
    DESTINATION ${CMAKE_INSTALL_PREFIX}/cmake
)

