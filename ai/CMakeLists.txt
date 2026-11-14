# =========================================================
# AI module
#   connectx_ai         search / learning based agents (depends on core only)
#   connectx_ai_client  network client that plays a game with an agent
#   executables         connectx-ai (bot), connectx-arena (tournaments),
#                       connectx-train (datasets, supervised + self-play training)
# =========================================================

option(CONNECTX_AI_POPCNT "Compile the AI module with the x86-64 POPCNT instruction" ON)

find_package(Threads REQUIRED)

# The from-scratch neural network library
add_subdirectory(nn)

# ---------------------------------------------------------
# Agents library
# ---------------------------------------------------------
add_library(connectx_ai STATIC
    src/rng.cpp
    src/position.cpp
    src/encoding.cpp
    src/evaluation.cpp
    src/solver.cpp
    src/arena.cpp
    src/factory.cpp
    src/model_cache.cpp
    src/alphazero.cpp
    src/agents/simple_agents.cpp
    src/agents/minimax_agent.cpp
    src/agents/alphabeta_agent.cpp
    src/agents/solver_agent.cpp
    src/agents/mcts_agent.cpp
    src/agents/neural_agent.cpp
    src/agents/neural_search_agent.cpp
    src/training/dataset.cpp
    src/training/trainer.cpp
    src/training/selfplay.cpp
)

add_library(Connectx::ai ALIAS connectx_ai)

target_include_directories(connectx_ai
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(connectx_ai PUBLIC Connectx::core Connectx::nn Threads::Threads)

target_compile_features(connectx_ai PUBLIC cxx_std_17)
set_target_properties(connectx_ai PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_EXTENSIONS OFF
)

# Population counts are everywhere in the bitboard code: use the hardware instruction when we can
if(CONNECTX_AI_POPCNT AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64" AND NOT MSVC)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-mpopcnt CONNECTX_HAS_MPOPCNT)

    if(CONNECTX_HAS_MPOPCNT)
        target_compile_options(connectx_ai PUBLIC -mpopcnt)
    endif()
endif()

# ---------------------------------------------------------
# Network client: a computer player for the server
# (needs the client library, so only when the network components are built)
# ---------------------------------------------------------
if(TARGET Connectx::client)
    add_library(connectx_ai_client STATIC src/bot_client.cpp)
    add_library(Connectx::ai_client ALIAS connectx_ai_client)

    target_link_libraries(connectx_ai_client PUBLIC Connectx::ai Connectx::client)
    target_compile_features(connectx_ai_client PUBLIC cxx_std_17)
    set_target_properties(connectx_ai_client PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_EXTENSIONS OFF)

    add_executable(connectx_ai_app app/ai_main.cpp)
    target_link_libraries(connectx_ai_app PRIVATE connectx_ai_client)
    set_target_properties(connectx_ai_app PROPERTIES OUTPUT_NAME "connectx-ai" CXX_EXTENSIONS OFF)

    install(TARGETS connectx_ai_app RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
else()
    message(STATUS "AI client skipped: the network components are not built")
endif()

# ---------------------------------------------------------
# Tournament runner
# ---------------------------------------------------------
add_executable(connectx_arena app/arena_main.cpp)
target_link_libraries(connectx_arena PRIVATE connectx_ai)
set_target_properties(connectx_arena PROPERTIES OUTPUT_NAME "connectx-arena" CXX_EXTENSIONS OFF)

install(TARGETS connectx_arena RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ---------------------------------------------------------
# Training tool: datasets, supervised learning, self-play, model utilities
# ---------------------------------------------------------
add_executable(connectx_train app/train_main.cpp)
target_link_libraries(connectx_train PRIVATE connectx_ai)
set_target_properties(connectx_train PROPERTIES OUTPUT_NAME "connectx-train" CXX_EXTENSIONS OFF)

install(TARGETS connectx_train RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ---------------------------------------------------------
# Pretrained models, next to the executables: "nn:model=connectx_6x7" finds <bin>/models/connectx_6x7.cxnn
# (the held-out test positions come along: connectx-train agent-accuracy --data models/test_positions_6x7.cxds)
# ---------------------------------------------------------
file(GLOB CONNECTX_MODELS ${CMAKE_CURRENT_SOURCE_DIR}/models/*.cxnn ${CMAKE_CURRENT_SOURCE_DIR}/models/*.cxds)

if(CONNECTX_MODELS)
    add_custom_target(connectx_models ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/models
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CONNECTX_MODELS} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/models
        COMMENT "Copying the pretrained models"
        VERBATIM)

    install(FILES ${CONNECTX_MODELS} DESTINATION ${CMAKE_INSTALL_BINDIR}/models)
endif()

# ---------------------------------------------------------
# Tests
# ---------------------------------------------------------
if(BUILD_TESTS)
    add_subdirectory(tests)
endif()
