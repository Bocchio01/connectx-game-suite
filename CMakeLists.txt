cmake_minimum_required(VERSION 3.16)
project(Connectx VERSION 3.0 LANGUAGES CXX)

# =========================================================
# Build options
# =========================================================
option(BUILD_CORE "Build core library" ON)
option(BUILD_NETWORK "Build network components" ON)
option(BUILD_CLI "Build CLI client" ON)
option(BUILD_GUI "Build GUI client" ON)
option(BUILD_AI "Build AI client" ON)
option(BUILD_GATEWAY "Build the web gateway (WebSocket front door of the server)" ON)

option(BUILD_TESTS "Build test executables" OFF)
option(BUILD_EXAMPLES "Build example executables" OFF)

option(GENERATE_DOCS "Generate documentation with Doxygen" OFF)

# =========================================================
# Global settings
# =========================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler warnings (will be inherited by all targets)
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# =========================================================
# Prevent in-source builds
# =========================================================
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not allowed. Please create a build directory.")
endif()

# =========================================================
# Output directories
# =========================================================
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# =========================================================
# Installation directories
# =========================================================
include(GNUInstallDirs)

# =========================================================
# FetchContent setup (centralized)
# =========================================================
include(FetchContent)

# Speed up FetchContent by using shallow clones
set(FETCHCONTENT_QUIET OFF)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON) # Don't update every time

# spdlog keeps a thread_local std::map (its "mapped diagnostic context") for every thread that logs.
# With MinGW-w64 the destructors of thread_local objects can run on memory that was already released when a
# thread ends, which crashes the program (heap corruption, exit code 0xC0000374): we have seen it with the
# short-lived threads that log, like the event thread of the client. Nothing here uses that context, so
# spdlog is built without thread-local storage (the only cost is that the thread id is not cached).
set(SPDLOG_NO_TLS ON CACHE BOOL "Build spdlog without thread-local storage (see CMakeLists.txt)" FORCE)

# Declare all dependencies upfront
message(STATUS "Fetching spdlog...")
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.16.0
    GIT_SHALLOW TRUE
)

if(BUILD_NETWORK)
    message(STATUS "Fetching sockpp...")
    FetchContent_Declare(
        sockpp
        GIT_REPOSITORY https://github.com/fpagliughi/sockpp.git
        GIT_TAG v0.8.3
        GIT_SHALLOW TRUE
    )

    message(STATUS "Fetching nlohmann_json...")
    FetchContent_Declare(
        json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.12.0
        GIT_SHALLOW TRUE
    )
endif()

# Make all declared dependencies available at once
set(_fetch_deps "")
list(APPEND _fetch_deps spdlog)

if(BUILD_NETWORK)
    list(APPEND _fetch_deps sockpp json)
endif()

if(_fetch_deps)
    message(STATUS "Making available FetchContent dependencies: ${_fetch_deps}")
    FetchContent_MakeAvailable(${_fetch_deps})
endif()

# Disable warnings for third-party libraries
if(BUILD_NETWORK AND TARGET sockpp-objs)
    message(STATUS "Disabling warnings for sockpp library")
    target_compile_options(sockpp-objs PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/w>
        $<$<CXX_COMPILER_ID:GNU>:-w>
        $<$<CXX_COMPILER_ID:Clang>:-w>
    )
endif()

if(BUILD_NETWORK AND TARGET sockpp)
    target_compile_options(sockpp PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/w>
        $<$<CXX_COMPILER_ID:GNU>:-w>
        $<$<CXX_COMPILER_ID:Clang>:-w>
    )

    # Mark sockpp includes as SYSTEM to suppress warnings in user code
    get_target_property(sockpp_include_dirs sockpp INTERFACE_INCLUDE_DIRECTORIES)

    if(sockpp_include_dirs)
        set_target_properties(sockpp PROPERTIES
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${sockpp_include_dirs}"
        )
    endif()
endif()

# =========================================================
# Find system packages
# =========================================================
if(BUILD_NETWORK)
    find_package(Threads REQUIRED)

    if(NOT Threads_FOUND)
        message(FATAL_ERROR "Threads library not found")
    endif()
endif()

# =========================================================
# Add subdirectories
# =========================================================

# Must happen before the subdirectories below: ai/ registers its own tests
if(BUILD_TESTS)
    enable_testing()
endif()

if(BUILD_CORE)
    message(STATUS "Adding core library...")
    add_subdirectory(core)
endif()

if(BUILD_NETWORK)
    message(STATUS "Adding network components...")
    add_subdirectory(network)
endif()

if(BUILD_CLI)
    message(STATUS "Adding CLI client...")
    add_subdirectory(ui/cli)
endif()

if(BUILD_GUI)
    message(STATUS "Adding GUI client...")
    add_subdirectory(ui/gui)
endif()

if(BUILD_AI)
    message(STATUS "Adding AI client...")
    add_subdirectory(ai)
endif()

# After the AI module: the gateway uses its computer player, when there is one
if(BUILD_GATEWAY)
    if(BUILD_NETWORK)
        message(STATUS "Adding web gateway...")
        add_subdirectory(network/gateway)
    else()
        message(WARNING "The web gateway needs the network components: skipping it")
    endif()
endif()

# =========================================================
# Testing setup
# =========================================================
if(BUILD_TESTS)
    enable_testing()
    message(STATUS "Testing enabled - use 'ctest' to run tests")
    add_subdirectory(tests)
endif()

# =========================================================
# Examples setup
# =========================================================
if(BUILD_EXAMPLES)
    message(STATUS "Adding examples...")
    add_subdirectory(examples)
endif()

# =========================================================
# Documentation setup
# =========================================================
find_package(Doxygen QUIET)

if(DOXYGEN_FOUND AND GENERATE_DOCS)
    FetchContent_Declare(
        doxygen-awesome-css
        URL https://github.com/jothepro/doxygen-awesome-css/archive/refs/heads/main.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(doxygen-awesome-css)
    FetchContent_GetProperties(doxygen-awesome-css SOURCE_DIR AWESOME_CSS_DIR)

    set(DOXYGEN_INPUT ${CMAKE_SOURCE_DIR}/docs/Doxyfile.in)
    set(DOXYGEN_OUTPUT ${CMAKE_BINARY_DIR}/docs/Doxyfile)
    configure_file(${DOXYGEN_INPUT} ${DOXYGEN_OUTPUT} @ONLY)

    add_custom_target(docs
        COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_OUTPUT}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Generating API documentation with Doxygen"
        VERBATIM
    )
elseif(GENERATE_DOCS AND NOT DOXYGEN_FOUND)
    message(WARNING "Doxygen not found. Install it to enable documentation generation.")
endif()

# =========================================================
# Print configuration summary
# =========================================================
message(STATUS "")
message(STATUS "========================================")
message(STATUS "         ConnectX Configuration         ")
message(STATUS "========================================")
message(STATUS " Version: ${PROJECT_VERSION}")
message(STATUS " Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS " C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "")
message(STATUS "Components:")
message(STATUS " - Core library:      ${BUILD_CORE}")
message(STATUS " - Network:           ${BUILD_NETWORK}")
message(STATUS " - CLI client:        ${BUILD_CLI}")
message(STATUS " - GUI client:        ${BUILD_GUI}")
message(STATUS " - AI client:         ${BUILD_AI}")
message(STATUS " - Web gateway:       ${BUILD_GATEWAY}")
message(STATUS "")
message(STATUS "Options:")
message(STATUS " - Build tests:       ${BUILD_TESTS}")
message(STATUS " - Build examples:    ${BUILD_EXAMPLES}")
message(STATUS "")
message(STATUS "Output:")
message(STATUS " - Executables:       ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS " - Libraries:         ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
message(STATUS "")
message(STATUS "Installation dirs:")
message(STATUS " - Binaries:          ${CMAKE_INSTALL_BINDIR}")
message(STATUS " - Libraries:         ${CMAKE_INSTALL_LIBDIR}")
message(STATUS " - Includes:          ${CMAKE_INSTALL_INCLUDEDIR}")
message(STATUS "========================================")
message(STATUS "")