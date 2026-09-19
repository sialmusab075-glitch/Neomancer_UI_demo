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
