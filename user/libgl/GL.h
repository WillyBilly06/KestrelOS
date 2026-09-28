/* GL.h - OpenGL for KestrelOS.
 *
 * This is OpenGL 1.x: a fixed-function pipeline driven by immediate mode and
 * vertex arrays, with a matrix stack, lighting, texturing and blending.  It is
 * the version worth implementing from nothing - complete enough to write a real
 * program against, small enough to be correct rather than merely present, and
 * the one that a fixed-function software rasteriser maps onto exactly.
 *
 * GPU-backed window targets use native GPU fixed-function triangle raster,
 * depth and texture sampling, plus the existing one-pixel point/line primitives.
 * Programmable array/indexed and immediate draws on GPU targets use GPU vertex and fragment
 * bytecode execution. Geometry preparation and clipping remain CPU work.
 * Immediate programmable draws use glVertex/attribute zero to submit a vertex,
 * with other generic attributes captured from glVertexAttrib values (not arrays).
 * Fixed-function GPU draws batch transforms, normals and lighting through the
 * GPU vertex VM, capturing materials per vertex. CPU targets retain a CPU path;
 * this is not complete OpenGL conformance or legacy GLSL built-in support.
 */
#ifndef KESTREL_GL_H
#define KESTREL_GL_H

#include <stdint.h>
#include <stddef.h>

typedef unsigned int   GLenum;
typedef unsigned char  GLboolean;
typedef unsigned int   GLbitfield;
typedef signed char    GLbyte;
typedef short          GLshort;
typedef int            GLint;
typedef int            GLsizei;
typedef unsigned char  GLubyte;
typedef unsigned short GLushort;
typedef unsigned int   GLuint;
typedef float          GLfloat;
typedef float          GLclampf;
typedef double         GLdouble;
typedef void           GLvoid;

#define GL_FALSE 0
#define GL_TRUE  1

/* Primitives. */
#define GL_POINTS         0x0000
#define GL_LINES          0x0001
#define GL_LINE_LOOP      0x0002
#define GL_LINE_STRIP     0x0003
#define GL_TRIANGLES      0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRIANGLE_FAN   0x0006
#define GL_QUADS          0x0007
#define GL_QUAD_STRIP     0x0008
#define GL_POLYGON        0x0009

/* Buffers to clear. */
#define GL_DEPTH_BUFFER_BIT   0x00000100
#define GL_STENCIL_BUFFER_BIT 0x00000400
#define GL_COLOR_BUFFER_BIT   0x00004000

/* Capabilities. */
#define GL_DEPTH_TEST     0x0B71
#define GL_CULL_FACE      0x0B44
#define GL_BLEND          0x0BE2
#define GL_TEXTURE_2D     0x0DE1
#define GL_LIGHTING       0x0B50
#define GL_LIGHT0         0x4000
#define GL_LIGHT1         0x4001
#define GL_LIGHT2         0x4002
#define GL_LIGHT3         0x4003
#define GL_NORMALIZE      0x0BA1
#define GL_SCISSOR_TEST   0x0C11
#define GL_ALPHA_TEST     0x0BC0
#define GL_COLOR_MATERIAL 0x0B57
#define GL_FOG            0x0B60
#define GL_DITHER         0x0BD0

/* Matrix modes. */
#define GL_MODELVIEW  0x1700
#define GL_PROJECTION 0x1701
#define GL_TEXTURE    0x1702

/* Comparison functions. */
#define GL_NEVER    0x0200
#define GL_LESS     0x0201
#define GL_EQUAL    0x0202
#define GL_LEQUAL   0x0203
#define GL_GREATER  0x0204
#define GL_NOTEQUAL 0x0205
#define GL_GEQUAL   0x0206
#define GL_ALWAYS   0x0207

/* Blend factors. */
#define GL_ZERO                0
#define GL_ONE                 1
#define GL_SRC_COLOR           0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA           0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA           0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR           0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307

/* Faces and winding. */
#define GL_FRONT          0x0404
#define GL_BACK           0x0405
#define GL_FRONT_AND_BACK 0x0408
#define GL_CW             0x0900
#define GL_CCW            0x0901

/* Shading. */
#define GL_FLAT   0x1D00
#define GL_SMOOTH 0x1D01

/* Types for vertex arrays and textures. */
#define GL_BYTE           0x1400
#define GL_UNSIGNED_BYTE  0x1401
#define GL_SHORT          0x1402
#define GL_UNSIGNED_SHORT 0x1403
#define GL_INT            0x1404
#define GL_UNSIGNED_INT   0x1405
#define GL_FLOAT          0x1406
#define GL_DOUBLE         0x140A

/* Texture formats and parameters. */
#define GL_RGB               0x1907
#define GL_RGBA              0x1908
#define GL_LUMINANCE         0x1909
#define GL_BGRA              0x80E1
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S     0x2802
#define GL_TEXTURE_WRAP_T     0x2803
#define GL_NEAREST            0x2600
#define GL_LINEAR             0x2601
#define GL_REPEAT             0x2901
#define GL_CLAMP              0x2900
#define GL_CLAMP_TO_EDGE      0x812F
#define GL_MODULATE           0x2100
#define GL_REPLACE            0x1E01
#define GL_DECAL              0x2101
#define GL_TEXTURE_ENV        0x2300
#define GL_TEXTURE_ENV_MODE   0x2200

/* Lighting parameters. */
#define GL_AMBIENT              0x1200
#define GL_DIFFUSE              0x1201
#define GL_SPECULAR             0x1202
#define GL_POSITION             0x1203
#define GL_SHININESS            0x1601
#define GL_EMISSION             0x1600
#define GL_AMBIENT_AND_DIFFUSE  0x1602
#define GL_LIGHT_MODEL_AMBIENT  0x0B53

/* Vertex arrays. */
#define GL_VERTEX_ARRAY        0x8074
#define GL_NORMAL_ARRAY        0x8075
#define GL_COLOR_ARRAY         0x8076
#define GL_TEXTURE_COORD_ARRAY 0x8078

/* Queries. */
#define GL_VENDOR     0x1F00
#define GL_RENDERER   0x1F01
#define GL_VERSION    0x1F02
#define GL_EXTENSIONS 0x1F03

/* Errors. */
#define GL_NO_ERROR          0
#define GL_INVALID_ENUM      0x0500
#define GL_INVALID_VALUE     0x0501
#define GL_INVALID_OPERATION 0x0502
#define GL_STACK_OVERFLOW    0x0503
#define GL_STACK_UNDERFLOW   0x0504
#define GL_OUT_OF_MEMORY     0x0505

/* --------------------------------------------------------------- the target */

/* OpenGL has no way of its own to say where the pixels go; on a desktop that is
 * the window system's job.  These two calls are that binding: point the
 * pipeline at a surface, and read back how big it is.
 *
 * The argument is a surface_t * from the toolkit, taken as void * so that a
 * program can include GL.h without pulling the whole toolkit in with it. */
void glSetTarget(void *colour_surface);
void glGetTargetSize(int *width, int *height);

/* ---------------------------------------------------------------- the calls */

void glClear(GLbitfield mask);
void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a);
void glClearDepth(GLdouble depth);

void glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h);
void glDepthRange(GLdouble near_val, GLdouble far_val);

void glEnable(GLenum cap);
void glDisable(GLenum cap);
GLboolean glIsEnabled(GLenum cap);

void glBegin(GLenum mode);
void glEnd(void);

void glVertex2f(GLfloat x, GLfloat y);
void glVertex3f(GLfloat x, GLfloat y, GLfloat z);
void glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w);
void glVertex3fv(const GLfloat *v);
void glVertex2i(GLint x, GLint y);
void glVertex3i(GLint x, GLint y, GLint z);

void glColor3f(GLfloat r, GLfloat g, GLfloat b);
void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void glColor3fv(const GLfloat *v);
void glColor4fv(const GLfloat *v);
void glColor3ub(GLubyte r, GLubyte g, GLubyte b);
void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a);

void glNormal3f(GLfloat x, GLfloat y, GLfloat z);
void glNormal3fv(const GLfloat *v);

void glTexCoord2f(GLfloat s, GLfloat t);
void glTexCoord2fv(const GLfloat *v);

/* Matrices. */
void glMatrixMode(GLenum mode);
void glLoadIdentity(void);
void glLoadMatrixf(const GLfloat *m);
void glMultMatrixf(const GLfloat *m);
void glPushMatrix(void);
void glPopMatrix(void);
void glTranslatef(GLfloat x, GLfloat y, GLfloat z);
void glScalef(GLfloat x, GLfloat y, GLfloat z);
void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f);
void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f);
void glGetFloatv(GLenum pname, GLfloat *out);

/* Fragment state. */
void glDepthFunc(GLenum func);
void glDepthMask(GLboolean flag);
void glBlendFunc(GLenum src, GLenum dst);
void glAlphaFunc(GLenum func, GLclampf ref);
void glCullFace(GLenum mode);
void glFrontFace(GLenum mode);
void glShadeModel(GLenum mode);
void glPolygonOffset(GLfloat factor, GLfloat units);
void glLineWidth(GLfloat width);
void glPointSize(GLfloat size);

/* Textures. */
void glGenTextures(GLsizei n, GLuint *out);
void glDeleteTextures(GLsizei n, const GLuint *ids);
void glBindTexture(GLenum target, GLuint id);
void glTexImage2D(GLenum target, GLint level, GLint internal_format,
                  GLsizei w, GLsizei h, GLint border,
                  GLenum format, GLenum type, const GLvoid *pixels);
void glTexParameteri(GLenum target, GLenum pname, GLint param);
void glTexEnvi(GLenum target, GLenum pname, GLint param);
GLboolean glIsTexture(GLuint id);

/* Lighting. */
void glLightfv(GLenum light, GLenum pname, const GLfloat *params);
void glLightf(GLenum light, GLenum pname, GLfloat param);
void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params);
void glMaterialf(GLenum face, GLenum pname, GLfloat param);
void glLightModelfv(GLenum pname, const GLfloat *params);

/* Vertex arrays. */
void glEnableClientState(GLenum array);
void glDisableClientState(GLenum array);
void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p);
void glNormalPointer(GLenum type, GLsizei stride, const GLvoid *p);
void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p);
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p);
void glDrawArrays(GLenum mode, GLint first, GLsizei count);
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices);

/* Odds and ends. */
void glFinish(void);
void glFlush(void);
GLenum glGetError(void);
const GLubyte *glGetString(GLenum name);

/* Triangles processed and CPU fragments written since the previous call.
 * GPU fragment counts are not queried; zero here does not mean zero GPU work. */
void glGetStats(unsigned *triangles, unsigned *fragments);

/* ------------------------------------------------------------------- GLU-ish */

void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble near_val, GLdouble far_val);
void gluLookAt(GLdouble ex, GLdouble ey, GLdouble ez,
               GLdouble cx, GLdouble cy, GLdouble cz,
               GLdouble ux, GLdouble uy, GLdouble uz);
void gluOrtho2D(GLdouble l, GLdouble r, GLdouble b, GLdouble t);

/* ------------------------------------------------------ the programmable path
 *
 * Everything drawn on hardware built since about 2004 is drawn by two small
 * programs the application supplies: one per vertex, one per pixel.  This is
 * the interface it does that through.  See shader.h for what is underneath.
 */
#define GL_FRAGMENT_SHADER   0x8B30
#define GL_VERTEX_SHADER     0x8B31
#define GL_COMPILE_STATUS    0x8B81
#define GL_LINK_STATUS       0x8B82
#define GL_INFO_LOG_LENGTH   0x8B84
#define GL_SHADER_TYPE       0x8B4F
#define GL_ACTIVE_UNIFORMS   0x8B86
#define GL_ACTIVE_ATTRIBUTES 0x8B89
#define GL_TEXTURE0          0x84C0
#define GL_TEXTURE1          0x84C1
#define GL_TEXTURE2          0x84C2
#define GL_TEXTURE3          0x84C3

GLuint glCreateShader(GLenum type);
void   glShaderSource(GLuint shader, GLsizei count, const char *const *string,
                      const GLint *length);
void   glCompileShader(GLuint shader);
void   glGetShaderiv(GLuint shader, GLenum pname, GLint *params);
void   glGetShaderInfoLog(GLuint shader, GLsizei cap, GLsizei *written, char *log);
void   glDeleteShader(GLuint shader);

GLuint glCreateProgram(void);
void   glAttachShader(GLuint program, GLuint shader);
void   glDetachShader(GLuint program, GLuint shader);
void   glLinkProgram(GLuint program);
void   glGetProgramiv(GLuint program, GLenum pname, GLint *params);
void   glGetProgramInfoLog(GLuint program, GLsizei cap, GLsizei *written, char *log);
void   glUseProgram(GLuint program);
void   glDeleteProgram(GLuint program);

GLint  glGetUniformLocation(GLuint program, const char *name);
GLint  glGetAttribLocation(GLuint program, const char *name);
void   glUniform1f(GLint location, GLfloat a);
void   glUniform2f(GLint location, GLfloat a, GLfloat b);
void   glUniform3f(GLint location, GLfloat a, GLfloat b, GLfloat c);
void   glUniform4f(GLint location, GLfloat a, GLfloat b, GLfloat c, GLfloat d);
void   glUniform1i(GLint location, GLint a);
void   glUniform1fv(GLint location, GLsizei count, const GLfloat *v);
void   glUniform2fv(GLint location, GLsizei count, const GLfloat *v);
void   glUniform3fv(GLint location, GLsizei count, const GLfloat *v);
void   glUniform4fv(GLint location, GLsizei count, const GLfloat *v);
void   glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose,
                          const GLfloat *value);

void   glEnableVertexAttribArray(GLuint index);
void   glDisableVertexAttribArray(GLuint index);
void   glVertexAttribPointer(GLuint index, GLint size, GLenum type,
                             GLboolean normalized, GLsizei stride,
                             const GLvoid *pointer);
void   glVertexAttrib3f(GLuint index, GLfloat x, GLfloat y, GLfloat z);
void   glVertexAttrib4f(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);

void   glActiveTexture(GLenum unit);

/* How much work the shaders did, for the same reason the triangle count is
 * reported: it is the number that says whether a frame is expensive. */
void   glGetShaderStats(unsigned *instructions);

#endif
