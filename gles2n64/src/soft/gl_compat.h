#ifndef SOFT_GL_COMPAT_H
#define SOFT_GL_COMPAT_H

/* GL scalar types the gles2n64 headers name; the soft build has no GL. */
#include <stdint.h>

typedef unsigned int   GLenum;
typedef unsigned char  GLboolean;
typedef unsigned int   GLbitfield;
typedef int8_t         GLbyte;
typedef uint8_t        GLubyte;
typedef int16_t        GLshort;
typedef uint16_t       GLushort;
typedef int            GLint;
typedef unsigned int   GLuint;
typedef int            GLsizei;
typedef float          GLfloat;
typedef float          GLclampf;
typedef void           GLvoid;

#endif
