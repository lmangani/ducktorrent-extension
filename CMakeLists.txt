cmake_minimum_required(VERSION 3.5)

# Project name
project(ducktorrent)

# Set C++ standard
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Define library sources and headers for udp-discovery
set(LIB_SOURCES
    udp-discovery-cpp/udp_discovery_ip_port.cpp
    udp-discovery-cpp/udp_discovery_peer.cpp
    udp-discovery-cpp/udp_discovery_protocol.cpp)

set(LIB_HEADERS
    udp-discovery-cpp/udp_discovery_ip_port.hpp
    udp-discovery-cpp/udp_discovery_peer.hpp
    udp-discovery-cpp/udp_discovery_peer_parameters.hpp
    udp-discovery-cpp/udp_discovery_protocol.hpp
    udp-discovery-cpp/udp_discovery_protocol_version.hpp)

# Create the udp-discovery library
add_library(udp-discovery STATIC ${LIB_SOURCES} ${LIB_HEADERS})
set_property(TARGET udp-discovery PROPERTY CXX_STANDARD 98)

# Include directories for the project
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/udp-discovery-cpp
    ${DUCKDB_INCLUDE_DIRS}  # Make sure this is set in your build environment
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
        udp-discovery
)

# Install the extension
install(TARGETS ${EXTENSION_NAME} udp-discovery
    EXPORT DuckDBExports
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

# Install export set
install(EXPORT DuckDBExports
    FILE DuckDBTargets.cmake
    DESTINATION ${CMAKE_INSTALL_PREFIX}/cmake)
