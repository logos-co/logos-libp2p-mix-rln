cmake_minimum_required(VERSION 3.14)
project(Libp2pMixRlnModulePlugin LANGUAGES CXX)

find_package(nlohmann_json REQUIRED)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()

logos_module(
    NAME libp2p_mix_rln_module
    SOURCES
        src/plugin.h
        src/config.h
        src/plugin.cpp
    EXTERNAL_LIBS
        libp2p_mix_rln
    # The nim-ffi generated header (lib/libp2p_mix_rln.h) uses CBOR marshalling,
    # matching the pattern of logos-libp2p-module.
    LINK_LIBRARIES
        tinycbor
)

target_link_libraries(libp2p_mix_rln_module_module_plugin PUBLIC nlohmann_json::nlohmann_json)
