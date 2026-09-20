cmake_minimum_required(VERSION 3.20)

project(SolSystemSim
    VERSION 0.1.0
    DESCRIPTION "Real-time Solar System simulator with a telemetry HUD"
    LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "SolSystemSim targets Windows 10/11 x64 only.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "SolSystemSim requires a 64-bit (x64) toolchain. "
                        "Use a 'x64 Native Tools' prompt or the '-A x64' generator platform.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Single-config generators (Ninja, MinGW Makefiles) default to Release.
get_property(SOLSIM_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT SOLSIM_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(SOLSIM_BUILD_TESTS "Build the simulation self-tests (sim_tests)" ON)

include(cmake/Dependencies.cmake)
add_subdirectory(external/glad)

# Warning policy for first-party targets only. Third-party targets are silenced
# in cmake/Dependencies.cmake, and their headers are included with <angle brackets>
# so /external:anglebrackets keeps them out of our /W4 output.
function(solsim_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /permissive- /utf-8
            /external:anglebrackets /external:W0)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Simulation library: pure C++17, no OpenGL / ImGui / GLFW / GLM.
# ---------------------------------------------------------------------------
add_library(solsim_sim STATIC
    src/sim/Vec3.h
    src/sim/Constants.h
    src/sim/OrbitalElements.h
    src/sim/Body.h
    src/sim/BodyTable.h
    src/sim/BodyTable.cpp
    src/sim/KeplerSolver.h
    src/sim/KeplerSolver.cpp
    src/sim/SolarSystem.h
    src/sim/SolarSystem.cpp
    src/sim/SimClock.h
    src/sim/SimClock.cpp
    src/sim/OrbitProbe.h
    src/sim/OrbitProbe.cpp
    src/sim/EventDetector.h
    src/sim/EventDetector.cpp)
target_include_directories(solsim_sim PUBLIC src)
solsim_set_warnings(solsim_sim)
set_target_properties(solsim_sim PROPERTIES FOLDER "solsim")

# ---------------------------------------------------------------------------
# NEO data layer: model, ingestion (parsing), storage, DSA, queries.
# Pure C++17 plus nlohmann/json and (later) SQLite. No OpenGL, ImGui or GLM, so
# the CLI tools and the tests link it without creating a window. It depends on
# solsim_sim for the Kepler solver and the calendar conversion.
# ---------------------------------------------------------------------------
add_library(solsim_neo STATIC
    src/neo/model/Asteroid.h
    src/neo/model/Asteroid.cpp
    src/neo/model/CloseApproach.h
    src/neo/model/Dataset.h
    src/neo/model/Dataset.cpp
    src/neo/model/JulianDate.h
    src/neo/model/JulianDate.cpp
    src/neo/model/Hash.h
    src/neo/model/Hash.cpp
    src/neo/storage/Database.h
    src/neo/storage/Database.cpp
    src/neo/dsa/Instrumentation.h
    src/neo/dsa/HashMap.h
    src/neo/dsa/Sort.h
    src/neo/dsa/BinaryHeap.h
    src/neo/dsa/AvlTree.h
    src/neo/dsa/BucketIndex.h
    src/neo/dsa/BucketIndex.cpp
    src/neo/query/Query.h
    src/neo/query/Query.cpp
    src/neo/query/Histogram.h
    src/neo/query/Histogram.cpp
    src/neo/query/NameIndex.h
    src/neo/query/NameIndex.cpp
    src/neo/query/IndexSet.h
    src/neo/query/IndexSet.cpp
    src/neo/query/QueryEngine.h
    src/neo/query/QueryEngine.cpp
    src/neo/ingest/ParseReport.h
    src/neo/ingest/ParseReport.cpp
    src/neo/ingest/SbdbParser.h
    src/neo/ingest/SbdbParser.cpp
    src/neo/ingest/CadParser.h
    src/neo/ingest/CadParser.cpp
    src/neo/ingest/detail/JsonTable.h
    src/neo/ingest/detail/JsonTable.cpp
    src/neo/ingest/HttpClient.h
    src/neo/ingest/ResponseCache.h
    src/neo/ingest/ResponseCache.cpp
    src/neo/ingest/Fetcher.h
    src/neo/ingest/Fetcher.cpp
    src/neo/ingest/Ingestor.h
    src/neo/ingest/Ingestor.cpp
    src/neo/ingest/IngestReport.cpp)
target_include_directories(solsim_neo PUBLIC src)
target_link_libraries(solsim_neo PUBLIC solsim_sim PRIVATE nlohmann_json sqlite3)
solsim_set_warnings(solsim_neo)
set_target_properties(solsim_neo PROPERTIES FOLDER "neo")

# Networking, kept in its own target so only the CLI can reach the network:
# tests and the application link solsim_neo and never see an IHttpClient
# implementation. WinHTTP is a Windows system library, not a dependency.
add_library(solsim_neo_net STATIC
    src/neo/net/WinHttpClient.h
    src/neo/net/WinHttpClient.cpp)
target_link_libraries(solsim_neo_net PUBLIC solsim_neo PRIVATE winhttp)
target_compile_definitions(solsim_neo_net PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
solsim_set_warnings(solsim_neo_net)
set_target_properties(solsim_neo_net PROPERTIES FOLDER "neo")

# ---------------------------------------------------------------------------
# Command-line tools
# ---------------------------------------------------------------------------
add_executable(neo_ingest tools/neo_ingest/main.cpp)
target_link_libraries(neo_ingest PRIVATE solsim_neo_net)
if(MINGW)
    target_link_options(neo_ingest PRIVATE -static)
endif()
solsim_set_warnings(neo_ingest)
set_target_properties(neo_ingest PROPERTIES FOLDER "neo")

# Console query tool: runs one Query against data/neo.db and prints the results
# table and the EXPLAIN. Reads the database only; never touches the network.
add_executable(neo_query tools/neo_query/main.cpp)
target_link_libraries(neo_query PRIVATE solsim_neo)
if(MINGW)
    target_link_options(neo_query PRIVATE -static)
endif()
solsim_set_warnings(neo_query)
set_target_properties(neo_query PROPERTIES FOLDER "neo")

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
find_package(OpenGL REQUIRED)

set(SOLSIM_APP_SOURCES
    src/main.cpp
    src/app/Application.h
    src/app/Application.cpp
    src/app/Window.h
    src/app/Window.cpp
    src/app/Input.h
    src/app/Input.cpp
    src/app/Paths.h
    src/app/Paths.cpp
    src/app/Log.h
    src/app/Log.cpp
    src/app/Screenshot.h
    src/app/Screenshot.cpp
    src/render/GlColor.h
    src/render/Shader.h
    src/render/Shader.cpp
    src/render/SphereMesh.h
    src/render/SphereMesh.cpp
    src/render/OrbitRenderer.h
    src/render/OrbitRenderer.cpp
    src/render/Camera.h
    src/render/Camera.cpp
    src/render/ScaleMapper.h
    src/render/ScaleMapper.cpp
    src/render/Picking.h
    src/render/Picking.cpp
    src/render/Starfield.h
    src/render/Starfield.cpp
    src/render/EclipticGrid.h
    src/render/EclipticGrid.cpp
    src/render/PointCloud.h
    src/render/PointCloud.cpp
    src/render/LineMesh.h
    src/render/LineMesh.cpp
    src/render/SceneRenderer.h
    src/render/SceneRenderer.cpp
    src/render/PostProcess.h
    src/render/PostProcess.cpp
    src/render/ReactorStructure.h
    src/render/ReactorStructure.cpp
    src/style/SceneStyle.h
    src/style/Palette.h
    src/hud/Theme.h
    src/hud/Theme.cpp
    src/hud/HudState.h
    src/hud/HudTheme.h
    src/hud/HudTheme.cpp
    src/hud/StatusStrips.cpp
    src/hud/HudLayout.h
    src/hud/HudLayout.cpp
    src/hud/Panels.h
    src/hud/TitleBar.cpp
    src/hud/Controls.cpp
    src/hud/PlanetPanel.cpp
    src/hud/DataGrid.cpp
    src/hud/HistoryPlot.h
    src/hud/HistoryPlot.cpp
    src/hud/LogPanel.h
    src/hud/LogPanel.cpp
    src/hud/SceneOverlay.h
    src/hud/SceneOverlay.cpp)

if(MSVC)
    # Declares PerMonitorV2 DPI awareness; merged into the exe by the linker.
    list(APPEND SOLSIM_APP_SOURCES src/app/solsim.manifest)
endif()

add_executable(solsim ${SOLSIM_APP_SOURCES})
target_link_libraries(solsim PRIVATE
    solsim_sim
    glad
    imgui
    implot
    glfw
    glm::glm
    OpenGL::GL)
target_compile_definitions(solsim PRIVATE
    GLFW_INCLUDE_NONE
    NOMINMAX
    WIN32_LEAN_AND_MEAN)
solsim_set_warnings(solsim)
set_target_properties(solsim PROPERTIES
    FOLDER "solsim"
    OUTPUT_NAME "SolSystemSim"
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:solsim>")

# Debug keeps the console for logging; every other config uses the WIN32 subsystem.
# main() stays the entry point in both cases.
if(MSVC)
    target_link_options(solsim PRIVATE
        "$<$<NOT:$<CONFIG:Debug>>:/SUBSYSTEM:WINDOWS;/ENTRY:mainCRTStartup>")
else()
    target_link_options(solsim PRIVATE "$<$<NOT:$<CONFIG:Debug>>:-mwindows>")
endif()

# MinGW-w64 (GCC): link the C++ runtime statically so SolSystemSim.exe runs from
# Explorer without libstdc++/libgcc/winpthread DLLs on PATH. The .manifest is
# MSVC-only; GLFW 3.4 enables per-monitor-v2 DPI awareness itself at runtime.
if(MINGW)
    target_link_options(solsim PRIVATE -static)
endif()

# Copy assets/ next to the executable so it runs from VS, the command line and Explorer.
add_custom_command(TARGET solsim POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:solsim>/assets"
    COMMENT "Copying assets next to SolSystemSim.exe"
    VERBATIM)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT solsim)

# ---------------------------------------------------------------------------
# Tests (simulation only; no window needed)
# ---------------------------------------------------------------------------
if(SOLSIM_BUILD_TESTS)
    enable_testing()
    add_executable(sim_tests tests/sim_tests.cpp)
    target_link_libraries(sim_tests PRIVATE solsim_sim)
    if(MINGW)
        target_link_options(sim_tests PRIVATE -static)
    endif()
    solsim_set_warnings(sim_tests)
    set_target_properties(sim_tests PROPERTIES FOLDER "solsim")
    add_test(NAME sim_tests COMMAND sim_tests)

    # Render geometry (scale mapping, picking, generators): GLM only, no GPU.
    add_executable(render_tests
        tests/render_tests.cpp
        src/render/Camera.cpp
        src/render/ScaleMapper.cpp
        src/render/Picking.cpp
        src/render/Starfield.cpp
        src/render/EclipticGrid.cpp
        src/render/ReactorStructure.cpp)
    target_link_libraries(render_tests PRIVATE solsim_sim glm::glm)
    if(MINGW)
        target_link_options(render_tests PRIVATE -static)
    endif()
    solsim_set_warnings(render_tests)
    set_target_properties(render_tests PROPERTIES FOLDER "solsim")
    add_test(NAME render_tests COMMAND render_tests)

    # NEO data layer: parsing, join and date helpers against saved JPL fixtures.
    # The fixture directory is compiled in so ctest can run from any working
    # directory; argv[1] overrides it.
    add_executable(neo_tests tests/neo_tests.cpp)
    target_link_libraries(neo_tests PRIVATE solsim_neo)
    target_compile_definitions(neo_tests PRIVATE
        NEO_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    if(MINGW)
        target_link_options(neo_tests PRIVATE -static)
    endif()
    solsim_set_warnings(neo_tests)
    set_target_properties(neo_tests PROPERTIES FOLDER "neo")
    add_test(NAME neo_tests COMMAND neo_tests)

    # The ingest pipeline (cache, retry/backoff, paging, windows, resume) driven
    # by a fake IHttpClient that serves the fixtures. Still no network.
    add_executable(neo_ingest_tests tests/neo_ingest_tests.cpp)
    target_link_libraries(neo_ingest_tests PRIVATE solsim_neo)
    target_compile_definitions(neo_ingest_tests PRIVATE
        NEO_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    if(MINGW)
        target_link_options(neo_ingest_tests PRIVATE -static)
    endif()
    solsim_set_warnings(neo_ingest_tests)
    set_target_properties(neo_ingest_tests PROPERTIES FOLDER "neo")
    add_test(NAME neo_ingest_tests COMMAND neo_ingest_tests)

    # SQLite persistence: round trip, integrity, schema version, atomic writes.
    add_executable(neo_storage_tests tests/neo_storage_tests.cpp)
    target_link_libraries(neo_storage_tests PRIVATE solsim_neo sqlite3)
    target_compile_definitions(neo_storage_tests PRIVATE
        NEO_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    if(MINGW)
        target_link_options(neo_storage_tests PRIVATE -static)
    endif()
    solsim_set_warnings(neo_storage_tests)
    set_target_properties(neo_storage_tests PROPERTIES FOLDER "neo")
    add_test(NAME neo_storage_tests COMMAND neo_storage_tests)

    # DSA structures: edge cases plus differential fuzzing against std::.
    # Run from the project root so the optional real-dataset pass can find
    # data/neo.db; it skips cleanly when there is none.
    add_executable(neo_dsa_tests tests/neo_dsa_tests.cpp)
    target_link_libraries(neo_dsa_tests PRIVATE solsim_neo)
    if(MINGW)
        target_link_options(neo_dsa_tests PRIVATE -static)
    endif()
    solsim_set_warnings(neo_dsa_tests)
    set_target_properties(neo_dsa_tests PROPERTIES FOLDER "neo")
    add_test(NAME neo_dsa_tests COMMAND neo_dsa_tests WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Query engine: validation, same-row semantics, the planner, and the oracle
    # (planned == fixed-order == naive for 10k+ random queries). Run from the
    # project root so the real-dataset pass can find data/neo.db.
    add_executable(neo_query_tests tests/neo_query_tests.cpp)
    target_link_libraries(neo_query_tests PRIVATE solsim_neo)
    target_compile_definitions(neo_query_tests PRIVATE
        NEO_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    if(MINGW)
        target_link_options(neo_query_tests PRIVATE -static)
    endif()
    solsim_set_warnings(neo_query_tests)
    set_target_properties(neo_query_tests PROPERTIES FOLDER "neo")
    add_test(NAME neo_query_tests COMMAND neo_query_tests WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()
