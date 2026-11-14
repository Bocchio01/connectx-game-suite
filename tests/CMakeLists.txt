# These tests use assert(): CMake defines NDEBUG in Release builds, which would turn every assert()
# into a no-op and make the tests pass without checking anything. Keep them active.
function(connectx_keep_asserts target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${target} PRIVATE -UNDEBUG)
    endif()
endfunction()

# ===================================================
# Core library tests
# ===================================================
file(GLOB CORE_TEST_SOURCES core/*.cpp)

foreach(test_src ${CORE_TEST_SOURCES})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name} PRIVATE connectx_core)
    connectx_keep_asserts(${test_name})
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# ===================================================
# Network tests
# ===================================================
if(BUILD_NETWORK)
    file(GLOB NETWORK_TEST_SOURCES network/*.cpp)

    foreach(test_src ${NETWORK_TEST_SOURCES})
        get_filename_component(test_name ${test_src} NAME_WE)
        add_executable(${test_name} ${test_src})
        target_link_libraries(${test_name} PRIVATE connectx_core connectx_network)
        connectx_keep_asserts(${test_name})
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
endif()
