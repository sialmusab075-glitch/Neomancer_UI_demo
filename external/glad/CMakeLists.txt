# Pre-generated OpenGL 3.3 core loader (no extensions). See tools/gen_glad.mjs.
add_library(glad STATIC
    src/gl.c
    include/glad/gl.h
    include/KHR/khrplatform.h)
target_include_directories(glad SYSTEM PUBLIC include)
if(MSVC)
    target_compile_options(glad PRIVATE /W0)
else()
    target_compile_options(glad PRIVATE -w)
endif()
set_target_properties(glad PROPERTIES FOLDER "external")
