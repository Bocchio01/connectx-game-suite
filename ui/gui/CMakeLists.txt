# ---------------------------------------------------------
# GUI client (Qt 6 Widgets)
#
# This is a normal sub-directory of the root project (see BUILD_GUI there): it has no
# project() of its own and relies on the targets Connectx::client, Connectx::core,
# Connectx::protocol and spdlog::spdlog defined by the root.
# ---------------------------------------------------------
option(CONNECTX_GUI_REQUIRED "Fail the configuration when Qt 6 is not found, instead of skipping the GUI client" OFF)

find_package(Qt6 6.2 QUIET COMPONENTS Widgets Network)

if(NOT Qt6_FOUND)
    # The GUI is built by default, but nobody should lose the server, the CLI and the AI because Qt is
    # not installed: say loudly what is missing and carry on without the GUI.
    set(_connectx_gui_qt_help
        "Qt 6 (>= 6.2, Widgets and Network modules) was not found.\n"
        "Point CMAKE_PREFIX_PATH to a Qt 6 kit, e.g. -DCMAKE_PREFIX_PATH=C:/Qt/6.9.3/mingw_64 "
        "(or ~/Qt/6.9.3/gcc_64).")
    if(CONNECTX_GUI_REQUIRED)
        message(FATAL_ERROR ${_connectx_gui_qt_help})
    endif()
    message(WARNING ${_connectx_gui_qt_help}
        "\nThe GUI client is NOT built. Silence this with -DBUILD_GUI=OFF, "
        "or make it an error with -DCONNECTX_GUI_REQUIRED=ON.")
    return()
endif()

message(STATUS "GUI client: using Qt ${Qt6_VERSION}")

# Qt's code generators (scoped to this directory only). Nothing here rewrites the sources.
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

option(CONNECTX_GUI_DEPLOY_QT "Run windeployqt after building the GUI, to copy the Qt runtime next to it (Windows only)" OFF)

# ---------------------------------------------------------
# GUI library: everything but main(), shared by the executable and the tests.
# Headers are listed so that AUTOMOC processes them.
# (Target names are kept short on purpose: AUTOMOC repeats them in the object paths, and
#  deep build directories on Windows hit the 250 characters limit of CMake very quickly.)
# ---------------------------------------------------------
add_library(gui_lib STATIC
    # Pure logic (no widgets)
    include/gui/game_logic.hpp
    src/game_logic.cpp
    include/gui/metatypes.hpp
    include/gui/game_model.hpp
    src/game_model.cpp
    include/gui/player_style.hpp
    src/player_style.cpp

    # Network (Qt facade over the Client library)
    include/gui/network_controller.hpp
    src/network_controller.cpp

    # Helper programs run as child processes (the computer opponent, the private server of offline games):
    # the GUI does not link the AI library, it runs connectx-ai and connectx-server like a user would
    include/gui/computer_levels.hpp
    src/computer_levels.cpp
    include/gui/helper_programs.hpp
    src/helper_programs.cpp
    include/gui/helper_process.hpp
    src/helper_process.cpp
    include/gui/computer_player.hpp
    src/computer_player.cpp
    include/gui/private_server.hpp
    src/private_server.cpp

    # Widgets and pages
    include/gui/ui_utils.hpp
    src/ui_utils.cpp
    include/gui/disc_painter.hpp
    src/disc_painter.cpp
    include/gui/board_widget.hpp
    src/board_widget.cpp
    include/gui/game_list_model.hpp
    src/game_list_model.cpp
    include/gui/new_game_dialog.hpp
    src/new_game_dialog.cpp
    include/gui/connect_page.hpp
    src/connect_page.cpp
    include/gui/lobby_page.hpp
    src/lobby_page.cpp
    include/gui/game_page.hpp
    src/game_page.cpp
    include/gui/main_window.hpp
    src/main_window.cpp
)

add_library(Connectx::gui_lib ALIAS gui_lib)

target_include_directories(gui_lib
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(gui_lib
    PUBLIC
    Qt6::Widgets
    Qt6::Network # (only to find a free port and to probe the private server of offline games)
    Connectx::client # also brings Connectx::core, Connectx::protocol, sockpp and spdlog
    spdlog::spdlog
)

target_compile_features(gui_lib PUBLIC cxx_std_17)
set_target_properties(gui_lib PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_EXTENSIONS OFF
)

# Keep Qt strict: no implicit char* <-> QString conversions, no deprecated API (Qt >= 6.2 only).
target_compile_definitions(gui_lib
    PRIVATE
    QT_NO_CAST_FROM_ASCII
    QT_NO_CAST_TO_ASCII
    QT_NO_FOREACH
    QT_NO_URL_CAST_FROM_STRING
    QT_DISABLE_DEPRECATED_BEFORE=0x060200
)

if(WIN32)
    # <windows.h> comes in through sockpp: keep min()/max() usable
    target_compile_definitions(gui_lib PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(MSVC)
    # Sources contain UTF-8 characters (×, …, ·)
    target_compile_options(gui_lib PUBLIC /utf-8)
endif()

# ---------------------------------------------------------
# GUI client executable
# ---------------------------------------------------------
qt_add_executable(connectx_gui WIN32 MACOSX_BUNDLE
    app/main.cpp
)

target_link_libraries(connectx_gui PRIVATE
    Connectx::gui_lib
)

target_compile_definitions(connectx_gui PRIVATE
    CONNECTX_GUI_VERSION="${PROJECT_VERSION}"
)

set_target_properties(connectx_gui PROPERTIES
    CXX_EXTENSIONS OFF
    OUTPUT_NAME "connectx-gui"
    MACOSX_BUNDLE_BUNDLE_NAME "ConnectX"
    MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.bocchio01.connectx"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
)

# ---------------------------------------------------------
# Optional: copy the Qt runtime next to the executable (Windows)
# ---------------------------------------------------------
if(WIN32 AND CONNECTX_GUI_DEPLOY_QT)
    get_filename_component(_connectx_qt_prefix "${Qt6_DIR}/../../.." ABSOLUTE)
    find_program(CONNECTX_WINDEPLOYQT windeployqt HINTS "${_connectx_qt_prefix}/bin")

    if(CONNECTX_WINDEPLOYQT)
        add_custom_command(TARGET connectx_gui POST_BUILD
            COMMAND "${CONNECTX_WINDEPLOYQT}" --no-translations --no-system-d3d-compiler --no-opengl-sw "$<TARGET_FILE:connectx_gui>"
            COMMENT "Deploying the Qt runtime next to connectx-gui"
            VERBATIM
        )
    else()
        message(WARNING "CONNECTX_GUI_DEPLOY_QT is ON but windeployqt was not found under ${_connectx_qt_prefix}/bin")
    endif()
endif()

# ---------------------------------------------------------
# Installation (optional, controlled by parent)
# ---------------------------------------------------------
install(TARGETS connectx_gui
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# ---------------------------------------------------------
# Tests (only when the root project is configured with -DBUILD_TESTS=ON)
# ---------------------------------------------------------
if(BUILD_TESTS)
    add_subdirectory(tests)
endif()
