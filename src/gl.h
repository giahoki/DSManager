// Minimal OpenGL 3.3 loader: opengl32.dll exports only GL 1.1, the rest comes from glfwGetProcAddress.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <GL/gl.h>
#include <cstddef>

typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;

#define GL_ARRAY_BUFFER       0x8892
#define GL_STREAM_DRAW        0x88E0
#define GL_FRAGMENT_SHADER    0x8B30
#define GL_VERTEX_SHADER      0x8B31
#define GL_COMPILE_STATUS     0x8B81
#define GL_LINK_STATUS        0x8B82
#define GL_R8                 0x8229
#define GL_RED                0x1903
#define GL_CLAMP_TO_EDGE      0x812F
#define GL_TEXTURE0           0x84C0

#define GL_FUNCS \
    X(GLuint, glCreateShader, (GLenum)) \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*)) \
    X(void, glCompileShader, (GLuint)) \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint*)) \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) \
    X(GLuint, glCreateProgram, (void)) \
    X(void, glAttachShader, (GLuint, GLuint)) \
    X(void, glLinkProgram, (GLuint)) \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint*)) \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) \
    X(void, glDeleteShader, (GLuint)) \
    X(void, glUseProgram, (GLuint)) \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar*)) \
    X(void, glUniform1f, (GLint, GLfloat)) \
    X(void, glUniform2f, (GLint, GLfloat, GLfloat)) \
    X(void, glUniform3f, (GLint, GLfloat, GLfloat, GLfloat)) \
    X(void, glUniform1i, (GLint, GLint)) \
    X(void, glGenVertexArrays, (GLsizei, GLuint*)) \
    X(void, glBindVertexArray, (GLuint)) \
    X(void, glGenBuffers, (GLsizei, GLuint*)) \
    X(void, glBindBuffer, (GLenum, GLuint)) \
    X(void, glBufferData, (GLenum, GLsizeiptr, const void*, GLenum)) \
    X(void, glEnableVertexAttribArray, (GLuint)) \
    X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)) \
    X(void, glActiveTexture, (GLenum))

#define X(ret, name, args) typedef ret(APIENTRY* PFN_##name##_t) args; extern PFN_##name##_t name;
GL_FUNCS
#undef X

using GLGetProc = void* (*)(const char*);
bool glLoad(GLGetProc getProc);
