# ===================================================
# AI module tests (plain executables registered with CTest)
# ===================================================
file(GLOB AI_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)

foreach(test_src ${AI_TEST_SOURCES})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(ai_${test_name} ${test_src})
    target_link_libraries(ai_${test_name} PRIVATE connectx_ai)

    # test_arduino_board.cpp compiles the header of the Arduino sketch
    target_include_directories(ai_${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../arduino/ConnectXNet)

    add_test(NAME ai_${test_name} COMMAND ai_${test_name})
endforeach()

# The Python tests need NumPy only (no TensorFlow): run them with CTest too, when Python is available
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    add_test(NAME ai_python_cxnn
        COMMAND ${Python3_EXECUTABLE} -m unittest discover -s tests
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../python)
    set_tests_properties(ai_python_cxnn PROPERTIES SKIP_REGULAR_EXPRESSION "No module named 'numpy'")
endif()
