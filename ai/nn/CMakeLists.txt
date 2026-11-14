# ---------------------------------------------------------
# nn: a small neural network library written from scratch
# (matrices, layers with manual backpropagation, losses, SGD/Adam,
# a two-headed policy/value network and its model file format).
# It knows nothing about Connect-X and depends on the standard library only.
# ---------------------------------------------------------
set(CONNECTX_NN_SOURCES
    src/matrix.cpp
    src/layers.cpp
    src/loss.cpp
    src/optimizer.cpp
    src/network.cpp
)

add_library(connectx_nn STATIC ${CONNECTX_NN_SOURCES})
add_library(Connectx::nn ALIAS connectx_nn)

target_include_directories(connectx_nn
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(connectx_nn PUBLIC cxx_std_17)
set_target_properties(connectx_nn PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_EXTENSIONS OFF
)

# ---------------------------------------------------------
# Tests
# ---------------------------------------------------------
if(BUILD_TESTS)
    # Behaviour of the library in its normal (float) configuration
    add_executable(nn_test_basics tests/test_basics.cpp)
    target_link_libraries(nn_test_basics PRIVATE connectx_nn)
    target_include_directories(nn_test_basics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../tests)
    add_test(NAME nn_test_basics COMMAND nn_test_basics)

    # Gradient checking: the library sources are compiled again with `double` as scalar type, so
    # that the comparison between analytic and numerical gradients is not polluted by float rounding
    add_executable(nn_test_gradients tests/test_gradients.cpp ${CONNECTX_NN_SOURCES})
    target_include_directories(nn_test_gradients PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../tests)
    target_compile_definitions(nn_test_gradients PRIVATE NN_REAL_TYPE=double)
    target_compile_features(nn_test_gradients PRIVATE cxx_std_17)
    add_test(NAME nn_test_gradients COMMAND nn_test_gradients)
endif()
