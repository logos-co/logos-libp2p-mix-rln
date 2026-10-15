cmake_minimum_required(VERSION 3.14)
project(MixRlnModuleTests LANGUAGES CXX)

include(LogosTest)

logos_test(
    NAME mix_rln_config_tests
    TEST_SOURCES unit_config.cpp
)

find_library(MIX_RLN_LIBRARY NAMES libp2p_mix_rln
    PATHS "${CMAKE_CURRENT_SOURCE_DIR}/../lib" NO_DEFAULT_PATH REQUIRED)
logos_test(
    NAME mix_rln_status_tests
    MODULE_SOURCES ../src/plugin.cpp ../src/rln_bridge.cpp
    TEST_SOURCES unit_status.cpp
    EXTRA_INCLUDES ../lib ../src
    EXTRA_LINK_LIBS ${MIX_RLN_LIBRARY} tinycbor
)
