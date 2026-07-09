/*
 * GLES3/gl3.h — Shim header that provides OpenGL ES 3.0 function pointers
 * when birdie-gui is built with the built-in GL loader (BD_GL_LOADER_BUILTIN).
 *
 * When the built-in loader is enabled, this shim sits on the include path
 * ahead of the system / Khronos header. It includes the real header to get the
 * GL types and enums but provides the function entry points via bd_gl.c's
 * function pointer table instead of extern prototypes. This lets the backend's
 * gl* call sites (glGenVertexArrays, glCreateShader, etc.) remain unchanged
 * while moving resolution from link-time (system linker) to run-time (bd_gl_load).
 *
 * When BD_GL_LOADER_EXTERNAL is set, this shim simply includes the real
 * Khronos header without any redirection, assuming the application has made
 * the GL entry points available through some other means (GLEW, GLAD, Galogen,
 * direct linking, or a custom loader).
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#ifndef BD_GLES3_GL3_H_SHIM
#define BD_GLES3_GL3_H_SHIM 1

#ifdef BD_GL_LOADER_BUILTIN

/* Built-in loader mode: use function pointers. */

/* Prevent the Khronos header from declaring extern gl* prototypes; we supply
 * them as function pointers instead. */
#define GL_GLES_PROTOTYPES 0

/* Fetch the real Khronos header for types, enums, and PFNGL* typedefs. */
#include_next <GLES3/gl3.h>

/* Forward declarations: the bd_gl.c function pointer table. */
extern PFNGLGENVERTEXARRAYSPROC bd_glad_glGenVertexArrays;
extern PFNGLGENBUFFERSPROC bd_glad_glGenBuffers;
extern PFNGLBINDVERTEXARRAYPROC bd_glad_glBindVertexArray;
extern PFNGLBINDBUFFERPROC bd_glad_glBindBuffer;
extern PFNGLENABLEVERTEXATTRIBARRAYPROC bd_glad_glEnableVertexAttribArray;
extern PFNGLVERTEXATTRIBPOINTERPROC bd_glad_glVertexAttribPointer;
extern PFNGLVIEWPORTPROC bd_glad_glViewport;
extern PFNGLCLEARCOLORPROC bd_glad_glClearColor;
extern PFNGLCLEARPROC bd_glad_glClear;
extern PFNGLCREATESHADERPROC bd_glad_glCreateShader;
extern PFNGLSHADERSOURCEPROC bd_glad_glShaderSource;
extern PFNGLCOMPILESHADERPROC bd_glad_glCompileShader;
extern PFNGLGETSHADERIVPROC bd_glad_glGetShaderiv;
extern PFNGLGETSHADERINFOLOGPROC bd_glad_glGetShaderInfoLog;
extern PFNGLDELETESHADERPROC bd_glad_glDeleteShader;
extern PFNGLCREATEPROGRAMPROC bd_glad_glCreateProgram;
extern PFNGLATTACHSHADERPROC bd_glad_glAttachShader;
extern PFNGLBINDATTRIBLOCATIONPROC bd_glad_glBindAttribLocation;
extern PFNGLLINKPROGRAMPROC bd_glad_glLinkProgram;
extern PFNGLGETPROGRAMIVPROC bd_glad_glGetProgramiv;
extern PFNGLGETPROGRAMINFOLOGPROC bd_glad_glGetProgramInfoLog;
extern PFNGLDELETEPROGRAMPROC bd_glad_glDeleteProgram;
extern PFNGLUSEPROGRAMPROC bd_glad_glUseProgram;
extern PFNGLGETUNIFORMLOCATIONPROC bd_glad_glGetUniformLocation;
extern PFNGLUNIFORM1IPROC bd_glad_glUniform1i;
extern PFNGLUNIFORM1FPROC bd_glad_glUniform1f;
extern PFNGLUNIFORM2FPROC bd_glad_glUniform2f;
extern PFNGLUNIFORM3FPROC bd_glad_glUniform3f;
extern PFNGLUNIFORM4FPROC bd_glad_glUniform4f;
extern PFNGLUNIFORMMATRIX4FVPROC bd_glad_glUniformMatrix4fv;
extern PFNGLENABLEPROC bd_glad_glEnable;
extern PFNGLBLENDFUNCPROC bd_glad_glBlendFunc;
extern PFNGLDISABLEPROC bd_glad_glDisable;
extern PFNGLBUFFERDATAPROC bd_glad_glBufferData;
extern PFNGLBUFFERSUBDATAPROC bd_glad_glBufferSubData;
extern PFNGLDRAWARRAYSPROC bd_glad_glDrawArrays;
extern PFNGLGENTEXTURESPROC bd_glad_glGenTextures;
extern PFNGLBINDTEXTUREPROC bd_glad_glBindTexture;
extern PFNGLTEXIMAGE2DPROC bd_glad_glTexImage2D;
extern PFNGLTEXPARAMETERIPROC bd_glad_glTexParameteri;
extern PFNGLTEXSUBIMAGE2DPROC bd_glad_glTexSubImage2D;
extern PFNGLACTIVETEXTUREPROC bd_glad_glActiveTexture;
extern PFNGLDELETETEXTURESPROC bd_glad_glDeleteTextures;
extern PFNGLSCISSORPROC bd_glad_glScissor;

/* Redirect gl* calls to the function pointer table. */
#define glGenVertexArrays(...) bd_glad_glGenVertexArrays(__VA_ARGS__)
#define glGenBuffers(...) bd_glad_glGenBuffers(__VA_ARGS__)
#define glBindVertexArray(...) bd_glad_glBindVertexArray(__VA_ARGS__)
#define glBindBuffer(...) bd_glad_glBindBuffer(__VA_ARGS__)
#define glEnableVertexAttribArray(...) bd_glad_glEnableVertexAttribArray(__VA_ARGS__)
#define glVertexAttribPointer(...) bd_glad_glVertexAttribPointer(__VA_ARGS__)
#define glViewport(...) bd_glad_glViewport(__VA_ARGS__)
#define glClearColor(...) bd_glad_glClearColor(__VA_ARGS__)
#define glClear(...) bd_glad_glClear(__VA_ARGS__)
#define glCreateShader(...) bd_glad_glCreateShader(__VA_ARGS__)
#define glShaderSource(...) bd_glad_glShaderSource(__VA_ARGS__)
#define glCompileShader(...) bd_glad_glCompileShader(__VA_ARGS__)
#define glGetShaderiv(...) bd_glad_glGetShaderiv(__VA_ARGS__)
#define glGetShaderInfoLog(...) bd_glad_glGetShaderInfoLog(__VA_ARGS__)
#define glDeleteShader(...) bd_glad_glDeleteShader(__VA_ARGS__)
#define glCreateProgram(...) bd_glad_glCreateProgram(__VA_ARGS__)
#define glAttachShader(...) bd_glad_glAttachShader(__VA_ARGS__)
#define glBindAttribLocation(...) bd_glad_glBindAttribLocation(__VA_ARGS__)
#define glLinkProgram(...) bd_glad_glLinkProgram(__VA_ARGS__)
#define glGetProgramiv(...) bd_glad_glGetProgramiv(__VA_ARGS__)
#define glGetProgramInfoLog(...) bd_glad_glGetProgramInfoLog(__VA_ARGS__)
#define glDeleteProgram(...) bd_glad_glDeleteProgram(__VA_ARGS__)
#define glUseProgram(...) bd_glad_glUseProgram(__VA_ARGS__)
#define glGetUniformLocation(...) bd_glad_glGetUniformLocation(__VA_ARGS__)
#define glUniform1i(...) bd_glad_glUniform1i(__VA_ARGS__)
#define glUniform1f(...) bd_glad_glUniform1f(__VA_ARGS__)
#define glUniform2f(...) bd_glad_glUniform2f(__VA_ARGS__)
#define glUniform3f(...) bd_glad_glUniform3f(__VA_ARGS__)
#define glUniform4f(...) bd_glad_glUniform4f(__VA_ARGS__)
#define glUniformMatrix4fv(...) bd_glad_glUniformMatrix4fv(__VA_ARGS__)
#define glEnable(...) bd_glad_glEnable(__VA_ARGS__)
#define glBlendFunc(...) bd_glad_glBlendFunc(__VA_ARGS__)
#define glDisable(...) bd_glad_glDisable(__VA_ARGS__)
#define glBufferData(...) bd_glad_glBufferData(__VA_ARGS__)
#define glBufferSubData(...) bd_glad_glBufferSubData(__VA_ARGS__)
#define glDrawArrays(...) bd_glad_glDrawArrays(__VA_ARGS__)
#define glGenTextures(...) bd_glad_glGenTextures(__VA_ARGS__)
#define glBindTexture(...) bd_glad_glBindTexture(__VA_ARGS__)
#define glTexImage2D(...) bd_glad_glTexImage2D(__VA_ARGS__)
#define glTexParameteri(...) bd_glad_glTexParameteri(__VA_ARGS__)
#define glTexSubImage2D(...) bd_glad_glTexSubImage2D(__VA_ARGS__)
#define glActiveTexture(...) bd_glad_glActiveTexture(__VA_ARGS__)
#define glDeleteTextures(...) bd_glad_glDeleteTextures(__VA_ARGS__)
#define glScissor(...) bd_glad_glScissor(__VA_ARGS__)

#else /* BD_GL_LOADER_EXTERNAL */

/* External loader mode: just include the real Khronos header as-is. The
 * application is responsible for making gl* symbols available. */
#include_next <GLES3/gl3.h>

#endif /* BD_GL_LOADER_BUILTIN */

#endif
