# ---------------------------------------------------------
# GUI tests (added by ui/gui/CMakeLists.txt when BUILD_TESTS=ON)
#
#   gui_test_logic    pure logic: winning lines, state diff, game model, player styles
#   gui_test_widgets  widgets rendered/driven offscreen: board, lobby, dialogs, game page
#   gui_test_network  NetworkController against a real connectx-server (started on a free port)
#   gui_test_flow     the whole MainWindow against a real connectx-server
#   gui_test_process  the helper processes (computer player, private server) against a stand-in program
#   gui_test_computer the whole MainWindow with the real connectx-ai (skipped when it is not there)
#   gui-gallery       not a test: renders the widgets to PNG files for visual checks
#   gui-fake-helper   not a test: the stand-in program of gui_test_process and gui_test_computer
#
# Target names are short on purpose (see the note in ../CMakeLists.txt); the executables get
# their descriptive names through OUTPUT_NAME.
# ---------------------------------------------------------
find_package(Qt6 6.2 QUIET COMPONENTS Test Network)

if(NOT Qt6Test_FOUND OR NOT Qt6Network_FOUND)
    message(WARNING "GUI tests need the Qt6 Test and Network modules: they are skipped.")
    return()
endif()

function(gui_add_test target output)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE gui_lib Qt6::Test Qt6::Network)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(${target} PROPERTIES OUTPUT_NAME ${output} CXX_EXTENSIONS OFF)

    # Tests that talk to a real server need the server executable (built first)
    if(TARGET connectx_server)
        add_dependencies(${target} connectx_server)
        target_compile_definitions(${target} PRIVATE CONNECTX_SERVER_EXE="$<TARGET_FILE:connectx_server>")
    endif()
endfunction()

function(gui_register_test target output timeout)
    add_test(NAME ${output} COMMAND ${target}) # (a target name: CMake substitutes the path of the executable)
    set_tests_properties(${output} PROPERTIES
        TIMEOUT ${timeout}
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
    # Windows has no rpath: let the test find the Qt DLLs (CMake >= 3.22)
    if(WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.22)
        get_filename_component(_qt_bin "${Qt6_DIR}/../../../bin" ABSOLUTE)
        set_property(TEST ${output} PROPERTY ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_qt_bin}")
    endif()
endfunction()

gui_add_test(gt_logic gui_test_logic test_logic.cpp)
gui_add_test(gt_widgets gui_test_widgets test_widgets.cpp)
gui_add_test(gt_network gui_test_network test_network.cpp test_support.hpp)
gui_add_test(gt_flow gui_test_flow test_flow.cpp test_support.hpp window_support.hpp)
gui_add_test(gt_gallery gui-gallery gallery.cpp)

# The helper programs of the offline features (connectx-ai, connectx-server) are run as child processes.
# gui-fake-helper stands in for them when a failure is the point (it cannot join a game, it never listens...).
add_executable(gt_fake fake_helper.cpp)
target_link_libraries(gt_fake PRIVATE Qt6::Core Qt6::Network)
target_compile_features(gt_fake PRIVATE cxx_std_17)
set_target_properties(gt_fake PROPERTIES OUTPUT_NAME gui-fake-helper CXX_EXTENSIONS OFF)

# gui_test_process: the helper processes (computer player, private server) against the stand-in.
# gui_test_computer: the whole window with the REAL connectx-ai and connectx-server, which must be next to the
# test binaries (a build with BUILD_AI=ON): without them every test of it is skipped, with a message.
gui_add_test(gt_proc gui_test_process test_process.cpp process_support.hpp)
gui_add_test(gt_cpu gui_test_computer test_computer.cpp process_support.hpp window_support.hpp test_support.hpp)
foreach(_target gt_proc gt_cpu)
    add_dependencies(${_target} gt_fake)
    target_compile_definitions(${_target} PRIVATE CONNECTX_FAKE_HELPER="$<TARGET_FILE:gt_fake>")
endforeach()

# The core library is only needed by the logic test, to cross-check the winning line detection
# against the rules used by the server.
target_link_libraries(gt_logic PRIVATE Connectx::core)

gui_register_test(gt_logic gui_test_logic 60)
gui_register_test(gt_widgets gui_test_widgets 120)
gui_register_test(gt_network gui_test_network 240)
gui_register_test(gt_flow gui_test_flow 240)
gui_register_test(gt_proc gui_test_process 180)
gui_register_test(gt_cpu gui_test_computer 600)
