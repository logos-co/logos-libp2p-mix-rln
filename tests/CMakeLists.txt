cmake_minimum_required(VERSION 3.14)
project(MixRlnModuleTests LANGUAGES CXX)

include(LogosTest)

logos_test(
    NAME mix_rln_config_tests
    TEST_SOURCES unit_config.cpp
)
