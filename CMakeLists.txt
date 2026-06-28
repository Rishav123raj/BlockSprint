cmake_minimum_required(VERSION 3.15)
project(CryptoTradingEngine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Configure include directories for local external dependencies
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/external
    ${CMAKE_CURRENT_SOURCE_DIR}/external/asio/asio/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/websocketpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Compiler definitions for WebSocket++ and standalone Asio
add_compile_definitions(
    ASIO_STANDALONE
    _WEBSOCKETPP_CPP11_STL_
)

# MSVC specific compiler flags for warning levels and optimization
if(MSVC)
    add_compile_options(/W4 /EHsc)
    add_compile_options($<$<CONFIG:Release>:/O2>)
else()
    add_compile_options(-Wall -Wextra -O3)
endif()

# Source Files
set(SRC_FILES
    src/order_book.cpp
    src/matching_engine.cpp
    src/api_server.cpp
)

# Main Executable
add_executable(crypto_matching_engine
    src/main.cpp
    ${SRC_FILES}
)

# Unit Tests Executable
add_executable(run_tests
    tests/main_test.cpp
    ${SRC_FILES}
)
