cmake_minimum_required(VERSION 3.23)
project(HFTSimulator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Build options ─────────────────────────────────────────────────────────────
option(BUILD_PYTHON_BINDINGS "Build pybind11 Python module (quantsim_core)" ON)
option(BUILD_TESTS           "Build C++ unit tests"                          ON)

# ── Find packages ─────────────────────────────────────────────────────────────
find_package(PkgConfig)

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
    src/bot_sim.cpp
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

# ── live_trader: C++-native Binance live (paper) trading (OpenSSL WSS) ─────────
find_package(OpenSSL QUIET)
if(NOT OpenSSL_FOUND AND EXISTS /opt/homebrew/opt/openssl@3)
    set(OPENSSL_ROOT_DIR /opt/homebrew/opt/openssl@3)
    find_package(OpenSSL QUIET)
endif()
if(OpenSSL_FOUND)
    add_executable(live_trader src/live_trader.cpp)
    target_include_directories(live_trader PRIVATE include third_party)
    target_link_libraries(live_trader PRIVATE OpenSSL::SSL OpenSSL::Crypto ${CMAKE_DL_LIBS})
    target_compile_options(live_trader PRIVATE -O3 -march=native -Wno-padded)
    message(STATUS "OpenSSL found — live_trader (C++ native Binance live) enabled")
else()
    message(WARNING "OpenSSL not found — live_trader (C++ live) skipped")
endif()

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

# ── (Retired GTK GUI removed — superseded by the Dear ImGui app below) ─────────

# ── QuantSim ImGui app (GLFW + OpenGL) — the new primary UI ────────────────────
if(PkgConfig_FOUND)
    pkg_check_modules(GLFW QUIET glfw3)
endif()
set(IMGUI_DIR  ${CMAKE_SOURCE_DIR}/third_party/imgui)
set(IMPLOT_DIR ${CMAKE_SOURCE_DIR}/third_party/implot)
if(GLFW_FOUND AND EXISTS ${IMGUI_DIR}/imgui.cpp AND EXISTS ${IMPLOT_DIR}/implot.cpp)
    set(IMGUI_SRC
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
        ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
        ${IMPLOT_DIR}/implot.cpp
        ${IMPLOT_DIR}/implot_items.cpp
        ${CMAKE_SOURCE_DIR}/third_party/ColorTextEdit/TextEditor.cpp
    )
    add_executable(quantsim src/app_imgui.cpp ${IMGUI_SRC})
    # The backtest engine (SimRunner, header-only in sim_core.hpp) compiles INTO
    # this target — so the app must be optimized or GUI-driven backtests run at -O0.
    target_compile_options(quantsim PRIVATE -O3 -march=native)
    target_include_directories(quantsim PRIVATE
        include ${IMGUI_DIR} ${IMGUI_DIR}/backends ${IMGUI_DIR}/misc/cpp ${IMPLOT_DIR}
        ${CMAKE_SOURCE_DIR}/third_party/ColorTextEdit
        ${CMAKE_SOURCE_DIR}/third_party/IconHeaders ${GLFW_INCLUDE_DIRS})
    target_compile_definitions(quantsim PRIVATE GL_SILENCE_DEPRECATION)
    target_link_directories(quantsim PRIVATE ${GLFW_LIBRARY_DIRS})
    target_link_libraries(quantsim PRIVATE quantsim_core_lib ${GLFW_LIBRARIES} "-framework OpenGL")
    # vendored imgui/implot: don't lint
    set_source_files_properties(${IMGUI_SRC} PROPERTIES COMPILE_FLAGS "-w")
    message(STATUS "GLFW found — quantsim ImGui app enabled")
else()
    message(STATUS "GLFW or vendored imgui/implot missing — quantsim app skipped")
endif()
