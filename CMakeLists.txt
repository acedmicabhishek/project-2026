cmake_minimum_required(VERSION 3.23)
project(HFTSimulator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Build options ─────────────────────────────────────────────────────────────
option(BUILD_PYTHON_BINDINGS "Build pybind11 Python module (quantsim_core)" ON)
option(BUILD_TESTS           "Build C++ unit tests"                          ON)
option(BUILD_GUI             "Build GTK GUI (requires gtkmm-4.0)"            ON)

# ── Find packages ─────────────────────────────────────────────────────────────
find_package(PkgConfig)
if(PkgConfig_FOUND AND BUILD_GUI)
    pkg_check_modules(GTKMM QUIET gtkmm-4.0)
endif()

# ── Core simulator library (original) ────────────────────────────────────────
add_library(hft_simulator
    src/market.cpp
    src/strategy.cpp
    src/engine.cpp
    src/benchmark.cpp
)
target_include_directories(hft_simulator PUBLIC include)
target_compile_options(hft_simulator PRIVATE -Wall -Wextra -Wpedantic)

# ── QuantSim core library (new LOB + matching + feed + risk) ──────────────────
add_library(quantsim_core_lib
    src/orderbook.cpp
    src/matching.cpp
    src/itch_parser.cpp
    src/feed_replay.cpp
)
target_include_directories(quantsim_core_lib PUBLIC include)
target_compile_options(quantsim_core_lib PRIVATE
    -Wall -Wextra -O3 -march=native
    # Suppress padding warning from ITCH pack(1) structs
    -Wno-padded
)

# ── Executables ───────────────────────────────────────────────────────────────
add_executable(run_simulation src/main.cpp)
target_link_libraries(run_simulation PRIVATE hft_simulator quantsim_core_lib)

# ── Python bindings ───────────────────────────────────────────────────────────
if(BUILD_PYTHON_BINDINGS)
    # Try to find pybind11 via cmake config (pip install pybind11 installs this)
    find_package(pybind11 CONFIG QUIET)
    if(NOT pybind11_FOUND)
        # Fallback: find via python
        execute_process(
            COMMAND python3 -c "import pybind11; print(pybind11.get_cmake_dir())"
            OUTPUT_VARIABLE pybind11_DIR
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(pybind11_DIR)
            find_package(pybind11 CONFIG REQUIRED HINTS "${pybind11_DIR}")
        endif()
    endif()

    if(pybind11_FOUND)
        pybind11_add_module(quantsim_core
            python/bindings.cpp
            src/orderbook.cpp
            src/matching.cpp
            src/itch_parser.cpp
            src/feed_replay.cpp
        )
        target_include_directories(quantsim_core PRIVATE include)
        target_compile_options(quantsim_core PRIVATE -O3 -march=native -Wno-padded)

        # Install at python/ level so `import quantsim_core` works when
        # python/ is on sys.path (standard for this project layout).
        install(TARGETS quantsim_core
                LIBRARY DESTINATION python)

        # Also copy into build tree for in-place use without `cmake --install`
        add_custom_command(TARGET quantsim_core POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
                $<TARGET_FILE:quantsim_core>
                ${CMAKE_SOURCE_DIR}/python/$<TARGET_FILE_NAME:quantsim_core>
            COMMENT "Copying quantsim_core to python/"
        )

        message(STATUS "pybind11 found — quantsim_core Python module will be built")
    else()
        message(WARNING "pybind11 not found — Python bindings skipped. "
                        "Install with: pip install pybind11")
    endif()
endif()

# ── Performance Benchmark ─────────────────────────────────────────────────────
add_executable(run_benchmark src/bench_lob.cpp)
target_link_libraries(run_benchmark PRIVATE quantsim_core_lib)
target_include_directories(run_benchmark PRIVATE include)
target_compile_options(run_benchmark PRIVATE -O3 -march=native -DNDEBUG -Wno-padded)

# ── C++ Tests ─────────────────────────────────────────────────────────────────
if(BUILD_TESTS)
    enable_testing()

    add_executable(test_orderbook tests/test_orderbook.cpp)
    target_link_libraries(test_orderbook PRIVATE quantsim_core_lib)
    target_include_directories(test_orderbook PRIVATE include)
    add_test(NAME OrderBookTests COMMAND test_orderbook)

    add_executable(test_matching tests/test_matching.cpp)
    target_link_libraries(test_matching PRIVATE quantsim_core_lib)
    target_include_directories(test_matching PRIVATE include)
    add_test(NAME MatchingTests COMMAND test_matching)
endif()

# ── GTK GUI (optional) ────────────────────────────────────────────────────────
if(BUILD_GUI AND GTKMM_FOUND)
    add_executable(hft_gui src/gui_main.cpp src/ui.cpp src/editor.cpp)
    target_include_directories(hft_gui PRIVATE include ${GTKMM_INCLUDE_DIRS})
    target_compile_options(hft_gui PRIVATE -Wall -Wextra -Wpedantic ${GTKMM_CFLAGS_OTHER})
    target_link_directories(hft_gui PRIVATE ${GTKMM_LIBRARY_DIRS})
    target_link_libraries(hft_gui PRIVATE hft_simulator quantsim_core_lib ${GTKMM_LIBRARIES} ${GTKMM_LDFLAGS_OTHER})
else()
    if(BUILD_GUI)
        message(STATUS "gtkmm not found — hft_gui will not be built")
    endif()
endif()
