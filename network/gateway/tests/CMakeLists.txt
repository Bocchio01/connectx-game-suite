# ===================================================
# Gateway tests (plain executables registered with CTest)
#   websocket    the WebSocket codec, on strings
#   gatekeeper   what a browser may send, and what it gets back
#   gateway      a real server and a real gateway talking to a WebSocket client, all inside the test
# ===================================================
file(GLOB GATEWAY_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)

foreach(test_src ${GATEWAY_TEST_SOURCES})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(gateway_${test_name} ${test_src})
    target_link_libraries(gateway_${test_name} PRIVATE connectx_gateway Connectx::server_lib)
    add_test(NAME gateway_${test_name} COMMAND gateway_${test_name})
endforeach()
