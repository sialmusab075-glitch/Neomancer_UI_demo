#pragma once

#include <glad/gl.h>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <string>
#include <unordered_map>

namespace render {

// GLSL program loaded from files. Uniform locations are cached by name.
class Shader {
public:
    Shader() = default;
    ~Shader();
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    // Paths are UTF-8. `geometryPath` is optional (empty = no geometry stage).
    // On failure, returns false and fills `error` with the compiler/linker log.
    bool loadFromFiles(const std::string& vertexPath, const std::string& fragmentPath, std::string& error,
                       const std::string& geometryPath = std::string());
    void destroy();

    void use() const;
    GLint location(const char* name) const;

    void set(const char* name, int v) const; // samplers
    void set(const char* name, float v) const;
    void set(const char* name, const glm::vec2& v) const;
    void set(const char* name, const glm::vec3& v) const;
    void set(const char* name, const glm::vec4& v) const;
    void set(const char* name, const glm::mat4& m) const;

private:
    GLuint program_ = 0;
    mutable std::unordered_map<std::string, GLint> locations_;
};

} // namespace render
