# Third-party dependencies, fetched at configure time and pinned to exact tags.
#
#   GLFW       3.4                 (tag commit a74efa0d5628b74adc0426af4c5710e287fa7c2c)
#   GLM        1.0.1               (tag commit 0af55ccecd98d4e5a8d1fad7de25ba429d60e863)
#   Dear ImGui v1.92.9b-docking    (tag commit b48d1afbe8ee8b238e2961dc363a949dd7304e23)
#   ImPlot     v1.0                (tag commit 524f9fcd48d76c13fdf94c5ffbba8787a1ff7e39)
#
# GLAD is not fetched: a pre-generated GL 3.3 core loader is committed in external/glad.

include(FetchContent)

# --- GLFW -------------------------------------------------------------------
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE)

# --- GLM (header-only) ------------------------------------------------------
set(GLM_BUILD_LIBRARY OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
    GIT_SHALLOW    TRUE)

# --- Dear ImGui (docking branch release tag) -------------------------------
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.9b-docking
    GIT_SHALLOW    TRUE)

# --- ImPlot -----------------------------------------------------------------
FetchContent_Declare(implot
    GIT_REPOSITORY https://github.com/epezent/implot.git
    GIT_TAG        v1.0
    GIT_SHALLOW    TRUE)

# ImGui and ImPlot ship no top-level CMakeLists.txt, so MakeAvailable only
# downloads them; the static targets are defined below.
FetchContent_MakeAvailable(glfw glm imgui implot)

# --- Vendored, committed dependencies (nothing is downloaded at build time) --
#
#   nlohmann/json 3.12.0   external/json/nlohmann/json.hpp  (MIT)
#   SQLite        3.53.4   external/sqlite/sqlite3.c/.h  (public domain)
#
# Versions, sources and verified hashes are recorded in THIRD_PARTY.md. Both are
# used only by the NEO data layer (src/neo), never by the renderer or the HUD.
# WinHTTP is a Windows system library: it needs no package, and only the
# networking target links it.

# Silence warnings coming from third-party code.
function(solsim_silence_target target)
    if(TARGET ${target})
        if(MSVC)
            target_compile_options(${target} PRIVATE /W0)
        else()
            target_compile_options(${target} PRIVATE -w)
        endif()
        set_target_properties(${target} PROPERTIES FOLDER "external")
    endif()
endfunction()

solsim_silence_target(glfw)
if(TARGET update_mappings)
    set_target_properties(update_mappings PROPERTIES FOLDER "external")
endif()

# --- imgui static library ---------------------------------------------------
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui SYSTEM PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_DEFINE_MATH_OPERATORS)
target_link_libraries(imgui PUBLIC glfw)
solsim_silence_target(imgui)

# --- implot static library (sibling of imgui) ------------------------------
add_library(implot STATIC
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
    ${implot_SOURCE_DIR}/implot_demo.cpp)
target_include_directories(implot SYSTEM PUBLIC ${implot_SOURCE_DIR})
target_link_libraries(implot PUBLIC imgui)
solsim_silence_target(implot)

# --- nlohmann/json (header only) --------------------------------------------
# SYSTEM include: keeps its headers out of /W4 and -Wall output.
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json SYSTEM INTERFACE
    ${PROJECT_SOURCE_DIR}/external/json)

# --- SQLite amalgamation ----------------------------------------------------
# Compile-time options:
#   THREADSAFE=2        multi-thread: one connection per thread, never shared.
#                       The ingest tool and the UI worker each open their own.
#   DQS=0               no double-quoted string literals: a misspelt column is
#                       an error instead of a silent string.
#   OMIT_LOAD_EXTENSION no dynamic extension loading (we never use it).
#   OMIT_DEPRECATED     drops the legacy API surface.
#   DEFAULT_MEMSTATUS=0 skips the allocation bookkeeping we don't read.
#   DEFAULT_FOREIGN_KEYS=1  approaches -> asteroids stays referentially sound.
add_library(sqlite3 STATIC ${PROJECT_SOURCE_DIR}/external/sqlite/sqlite3.c)
target_include_directories(sqlite3 SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/external/sqlite)
target_compile_definitions(sqlite3 PUBLIC
    SQLITE_THREADSAFE=2
    SQLITE_DQS=0
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_OMIT_DEPRECATED
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_DEFAULT_FOREIGN_KEYS=1)
solsim_silence_target(sqlite3)
