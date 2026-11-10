// OpenGL 1.x scalar types and enums used by the shim (no system GL headers: the shim is the implementation).
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef uint32_t GLenum; typedef uint8_t GLboolean; typedef uint32_t GLbitfield; typedef int8_t GLbyte;
typedef int16_t GLshort; typedef int32_t GLint; typedef int32_t GLsizei; typedef uint8_t GLubyte;
typedef uint16_t GLushort; typedef uint32_t GLuint; typedef float GLfloat; typedef float GLclampf;
typedef double GLdouble; typedef double GLclampd; typedef void GLvoid; typedef char GLchar; typedef char GLcharARB;
typedef long GLintptr; typedef long GLsizeiptr; typedef long GLintptrARB; typedef long GLsizeiptrARB;
typedef uint16_t GLhalf; typedef uint16_t GLhalfARB; typedef void *GLhandleARB; typedef int64_t GLint64EXT;
typedef uint64_t GLuint64EXT; typedef struct __GLsync *GLsync; typedef int64_t GLint64; typedef uint64_t GLuint64;
