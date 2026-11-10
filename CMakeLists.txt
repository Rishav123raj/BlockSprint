cmake_minimum_required(VERSION 3.15)
project(BlockSprint VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to Release for single-config generators (Ninja, Makefiles)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(BLOCKSPRINT_BUILD_SERVER "Build the live REST/WebSocket server" ON)
option(BLOCKSPRINT_BUILD_TESTS "Build the unit tests" ON)
option(BLOCKSPRINT_BUILD_BENCH "Build the engine micro-benchmark" ON)
option(BLOCKSPRINT_BUILD_SIM "Build the simulator (sim library, bs_sim, sim_tests)" ON)
option(BLOCKSPRINT_BUILD_PYTHON "Build the blocksprint_sim Python module (P4.1; pyproject.toml turns it on)" OFF)

if(BLOCKSPRINT_BUILD_PYTHON)
    # The static libraries end up inside a shared module
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

find_package(Threads REQUIRED)

# Warning flags applied to BlockSprint's own targets only (not vendored code)
function(blocksprint_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /EHsc)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# engine_core: matching engine library (no networking)
# ---------------------------------------------------------------------------
add_library(engine_core STATIC
    src/core/types.cpp
    src/core/symbol_spec.cpp
    src/core/fees.cpp
    src/core/order_book.cpp
    src/core/matching_engine.cpp
)
target_include_directories(engine_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
)
target_include_directories(engine_core SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/external
)
blocksprint_warnings(engine_core)

# ---------------------------------------------------------------------------
# api + crypto_matching_engine: live REST/WebSocket server
# ---------------------------------------------------------------------------
if(BLOCKSPRINT_BUILD_SERVER)
    add_library(api STATIC
        src/api/api_server.cpp
    )
    target_include_directories(api SYSTEM PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/external/asio/asio/include
        ${CMAKE_CURRENT_SOURCE_DIR}/external/websocketpp
    )
    # _WEBSOCKETPP_CPP11_THREAD_: websocketpp disables std::thread on MinGW by default
    target_compile_definitions(api PUBLIC
        ASIO_STANDALONE
        _WEBSOCKETPP_CPP11_STL_
        _WEBSOCKETPP_CPP11_THREAD_
    )
    target_link_libraries(api PUBLIC engine_core Threads::Threads)
    if(WIN32)
        target_link_libraries(api PUBLIC ws2_32 mswsock)
    endif()
    blocksprint_warnings(api)

    add_executable(crypto_matching_engine src/main.cpp)
    target_link_libraries(crypto_matching_engine PRIVATE api)
    blocksprint_warnings(crypto_matching_engine)
endif()

# ---------------------------------------------------------------------------
# unit_tests: doctest unit tests (link engine_core only)
# ---------------------------------------------------------------------------
if(BLOCKSPRINT_BUILD_TESTS)
    enable_testing()

    add_executable(unit_tests
        tests/core/test_main.cpp
        tests/core/matching_test.cpp
        tests/core/characterization_test.cpp
        tests/core/invariant_test.cpp
        tests/core/types_test.cpp
        tests/core/engine_test.cpp
        tests/core/stp_test.cpp
        tests/core/exec_report_test.cpp
        tests/core/validation_test.cpp
        tests/core/fees_test.cpp
        tests/core/post_only_amend_test.cpp
        tests/core/logger_test.cpp
        tests/core/fuzz_test.cpp
        tests/core/determinism_test.cpp
    )
    target_include_directories(unit_tests SYSTEM PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/external/doctest
    )
    target_link_libraries(unit_tests PRIVATE engine_core Threads::Threads)
    blocksprint_warnings(unit_tests)

    add_test(NAME unit_tests COMMAND unit_tests)
endif()

# ---------------------------------------------------------------------------
# sim + bs_sim + sim_tests: the market simulator (Phase 3; docs/simulator_architecture.md)
# ---------------------------------------------------------------------------
if(BLOCKSPRINT_BUILD_SIM)
    add_library(sim STATIC
        src/sim/bse.cpp
        src/sim/mmap_file.cpp
        src/sim/replayer.cpp
        src/sim/reconstruct.cpp
        src/sim/synthesizer.cpp
        src/sim/observed_book.cpp
        src/sim/gateway.cpp
        src/sim/simulator.cpp
        src/sim/config.cpp
        src/sim/recorder.cpp
        src/sim/run.cpp
        src/sim/strategies/registry.cpp
        src/sim/strategies/passive_quoter.cpp
        src/sim/strategies/twap_taker.cpp
    )
    target_include_directories(sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/sim)
    target_link_libraries(sim PUBLIC engine_core)
    blocksprint_warnings(sim)

    add_executable(bs_sim src/sim/main.cpp)
    target_link_libraries(bs_sim PRIVATE sim)
    # bs_sim run records the commit it was run from (git -C <source dir>)
    target_compile_definitions(bs_sim PRIVATE BLOCKSPRINT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    blocksprint_warnings(bs_sim)

    if(BLOCKSPRINT_BUILD_TESTS)
        add_executable(sim_tests
            tests/sim/test_main.cpp
            tests/sim/rng_queue_test.cpp
            tests/sim/replay_test.cpp
            tests/sim/synthesizer_test.cpp
            tests/sim/latency_observed_test.cpp
            tests/sim/gateway_test.cpp
            tests/sim/simulator_test.cpp
            tests/sim/config_test.cpp
            tests/sim/recorder_test.cpp
            tests/sim/strategies_test.cpp
            tests/sim/golden_test.cpp
            tests/sim/determinism_test.cpp
        )
        target_include_directories(sim_tests SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/external/doctest)
        target_link_libraries(sim_tests PRIVATE sim)
        target_compile_definitions(sim_tests PRIVATE
            BLOCKSPRINT_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/golden"
            BLOCKSPRINT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/data")
        blocksprint_warnings(sim_tests)
        add_test(NAME sim_tests COMMAND sim_tests)
    endif()
endif()

# ---------------------------------------------------------------------------
# blocksprint_sim: the simulator as a Python module (Phase 4; docs/dev_setup.md)
# ---------------------------------------------------------------------------
if(BLOCKSPRINT_BUILD_PYTHON)
    if(NOT BLOCKSPRINT_BUILD_SIM)
        message(FATAL_ERROR "BLOCKSPRINT_BUILD_PYTHON needs BLOCKSPRINT_BUILD_SIM")
    endif()
    set(PYBIND11_FINDPYTHON ON)
    find_package(Python 3.13 REQUIRED COMPONENTS Interpreter Development.Module)
    add_subdirectory(external/pybind11)
    pybind11_add_module(blocksprint_sim src/bindings/pybind_module.cpp)
    target_link_libraries(blocksprint_sim PRIVATE sim)
    target_compile_definitions(blocksprint_sim PRIVATE BLOCKSPRINT_VERSION="${PROJECT_VERSION}")
    blocksprint_warnings(blocksprint_sim)
    # scikit-build-core installs into the wheel's root: `import blocksprint_sim`
    install(TARGETS blocksprint_sim LIBRARY DESTINATION .)
endif()

# ---------------------------------------------------------------------------
# bench_core: engine-only micro-benchmark (no networking)
# ---------------------------------------------------------------------------
if(BLOCKSPRINT_BUILD_BENCH)
    add_executable(bench_core bench/bench_core.cpp)
    target_link_libraries(bench_core PRIVATE engine_core)
    blocksprint_warnings(bench_core)
    if(BLOCKSPRINT_BUILD_SIM)
        # P3.18: records per second through each layer of the simulator
        add_executable(bench_sim bench/bench_sim.cpp)
        target_link_libraries(bench_sim PRIVATE sim)
        blocksprint_warnings(bench_sim)
    endif()
endif()
