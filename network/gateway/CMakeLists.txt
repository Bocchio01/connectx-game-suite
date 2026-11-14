# =========================================================
# Web gateway
#   connectx_gateway  WebSocket front door of the game server, as a library
#   connectx-gateway  the program that runs it
#
# It is configured after the AI module (see the root CMakeLists.txt): the computer player of the web
# interface is the same one that the `connectx-ai` program uses.
# =========================================================

# ---------------------------------------------------------
# Library
# ---------------------------------------------------------
add_library(connectx_gateway STATIC
    src/websocket.cpp
    src/socket_util.cpp
    src/web_connection.cpp
    src/gatekeeper.cpp
    src/computer.cpp
    src/session.cpp
    src/gateway.cpp
)

add_library(Connectx::gateway ALIAS connectx_gateway)

target_include_directories(connectx_gateway
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# (sockpp, spdlog and Threads are fetched/found in the root)
target_link_libraries(connectx_gateway
    PUBLIC
    Connectx::protocol
    sockpp
    Threads::Threads
    spdlog::spdlog
)

if(WIN32)
    target_link_libraries(connectx_gateway PUBLIC ws2_32)
endif()

target_compile_features(connectx_gateway PUBLIC cxx_std_17)
set_target_properties(connectx_gateway PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_EXTENSIONS OFF
)

# Computer opponents for the browsers, when the AI module is there to provide them
if(TARGET Connectx::ai_client)
    target_link_libraries(connectx_gateway PUBLIC Connectx::ai_client)
    target_compile_definitions(connectx_gateway PUBLIC CONNECTX_GATEWAY_COMPUTER)
else()
    message(STATUS "Web gateway: built without computer opponents (the AI client is not built)")
endif()

# ---------------------------------------------------------
# Executable
# ---------------------------------------------------------
add_executable(connectx_gateway_app app/main.cpp)
target_link_libraries(connectx_gateway_app PRIVATE connectx_gateway)
set_target_properties(connectx_gateway_app PROPERTIES OUTPUT_NAME "connectx-gateway" CXX_EXTENSIONS OFF)

install(TARGETS connectx_gateway_app RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ---------------------------------------------------------
# Tests
# ---------------------------------------------------------
if(BUILD_TESTS)
    add_subdirectory(tests)
endif()
