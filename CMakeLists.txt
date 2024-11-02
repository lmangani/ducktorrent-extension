cmake_minimum_required(VERSION 3.5)

# Project name
project(ducktorrent)

set(CLIB dht)
file(GLOB CLIB_S "${CLIB}/*.c")
file(GLOB CLIB_H "${CLIB}/*.h")
add_library(${CLIB} STATIC ${CLIB_S} ${CLIB_H})
target_compile_options(${CLIB} PRIVATE -DCLIB)
target_link_libraries(${CLIB})

# Include directories for the project
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/dht
    ${DUCKDB_INCLUDE_DIRS}
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

# Link dependencies
target_link_libraries(${EXTENSION_NAME}
    PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
	${CLIB}
	libcrypt.so
)

# Install the extension
install(TARGETS ${EXTENSION_NAME}
    EXPORT DuckDBExports
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

# Install export set
install(EXPORT DuckDBExports
    FILE DuckDBTargets.cmake
    DESTINATION ${CMAKE_INSTALL_PREFIX}/cmake)
