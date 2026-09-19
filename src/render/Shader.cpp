#include "render/Shader.h"

#include "app/Paths.h"

#include <glm/gtc/type_ptr.hpp>

#include <cstddef>
#include <vector>

namespace render {

namespace {

std::string infoLog(GLuint object, bool isProgram) {
    GLint len = 0;
    if (isProgram) {
        glGetProgramiv(object, GL_INFO_LOG_LENGTH, &len);
    } else {
        glGetShaderiv(object, GL_INFO_LOG_LENGTH, &len);
    }
    if (len <= 1) {
        return std::string();
    }
    std::vector<char> buf(static_cast<std::size_t>(len));
    if (isProgram) {
        glGetProgramInfoLog(object, len, nullptr, buf.data());
    } else {
        glGetShaderInfoLog(object, len, nullptr, buf.data());
    }
    return std::string(buf.data());
}

GLuint compileFile(GLenum stage, const std::string& path, std::string& error) {
    std::string source;
    if (!app::readTextFile(path, source)) {
        error = "Missing shader file: " + path;
        return 0;
    }
    const GLuint shader = glCreateShader(stage);
    const char* src = source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        error = "Shader compile failed: " + path + "\n" + infoLog(shader, false);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

} // namespace

Shader::~Shader() { destroy(); }

void Shader::destroy() {
    if (program_) {
        glDeleteProgram(program_);
        program_ = 0;
    }
    locations_.clear();
}

bool Shader::loadFromFiles(const std::string& vertexPath, const std::string& fragmentPath, std::string& error,
                           const std::string& geometryPath) {
    destroy();

    GLuint stages[3] = {0, 0, 0};
    const auto cleanup = [&stages]() {
        for (GLuint s : stages) {
            if (s) glDeleteShader(s);
        }
    };

    stages[0] = compileFile(GL_VERTEX_SHADER, vertexPath, error);
    if (!stages[0]) {
        cleanup();
        return false;
    }
    stages[1] = compileFile(GL_FRAGMENT_SHADER, fragmentPath, error);
    if (!stages[1]) {
        cleanup();
        return false;
    }
    if (!geometryPath.empty()) {
        stages[2] = compileFile(GL_GEOMETRY_SHADER, geometryPath, error);
        if (!stages[2]) {
            cleanup();
            return false;
        }
    }

    program_ = glCreateProgram();
    for (GLuint s : stages) {
        if (s) glAttachShader(program_, s);
    }
    glLinkProgram(program_);
    cleanup();

    GLint ok = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        error = "Shader link failed: " + vertexPath + " + " + fragmentPath + "\n" + infoLog(program_, true);
        destroy();
        return false;
    }
    return true;
}

void Shader::use() const { glUseProgram(program_); }

GLint Shader::location(const char* name) const {
    auto it = locations_.find(name);
    if (it != locations_.end()) {
        return it->second;
    }
    const GLint loc = glGetUniformLocation(program_, name);
    locations_.emplace(name, loc);
    return loc;
}

void Shader::set(const char* name, int v) const { glUniform1i(location(name), v); }

void Shader::set(const char* name, float v) const { glUniform1f(location(name), v); }

void Shader::set(const char* name, const glm::vec2& v) const {
    glUniform2fv(location(name), 1, glm::value_ptr(v));
}

void Shader::set(const char* name, const glm::vec3& v) const {
    glUniform3fv(location(name), 1, glm::value_ptr(v));
}

void Shader::set(const char* name, const glm::vec4& v) const {
    glUniform4fv(location(name), 1, glm::value_ptr(v));
}

void Shader::set(const char* name, const glm::mat4& m) const {
    glUniformMatrix4fv(location(name), 1, GL_FALSE, glm::value_ptr(m));
}

} // namespace render
