/*
 * bd_gl.c — OpenGL ES 3.0 function pointer loader.
 *
 * Resolves GLES entry points from a getproc callback. This decouples birdie-gui
 * from how GL symbols are made available (system linker, GLEW, GLAD, Galogen,
 * SDL_GL_GetProcAddress, eglGetProcAddress, or a custom shim).
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#define GL_GLES_PROTOTYPES 0
#include <GLES3/gl3.h>
#include <stdio.h>

/* Define storage for each function pointer birdie-gui uses. */

#define PFNGLGENVERTEXARRAYSPROC_bd PFNGLGENVERTEXARRAYSPROC
#define PFNGLGENBUFFERSPROC_bd PFNGLGENBUFFERSPROC
#define PFNGLBINDVERTEXARRAYPROC_bd PFNGLBINDVERTEXARRAYPROC
#define PFNGLBINDBUFFERPROC_bd PFNGLBINDBUFFERPROC
#define PFNGLENABLEVERTEXATTRIBARRAYPROC_bd PFNGLENABLEVERTEXATTRIBARRAYPROC
#define PFNGLVERTEXATTRIBPOINTERPROC_bd PFNGLVERTEXATTRIBPOINTERPROC
#define PFNGLVIEWPORTPROC_bd PFNGLVIEWPORTPROC
#define PFNGLCLEARCOLORPROC_bd PFNGLCLEARCOLORPROC
#define PFNGLCLEARPROC_bd PFNGLCLEARPROC
#define PFNGLCREATESHADERPROC_bd PFNGLCREATESHADERPROC
#define PFNGLSHADERSOURCEPROC_bd PFNGLSHADERSOURCEPROC
#define PFNGLCOMPILESHADERPROC_bd PFNGLCOMPILESHADERPROC
#define PFNGLGETSHADERIVPROC_bd PFNGLGETSHADERIVPROC
#define PFNGLGETSHADERINFOLOGPROC_bd PFNGLGETSHADERINFOLOGPROC
#define PFNGLDELETESHADERPROC_bd PFNGLDELETESHADERPROC
#define PFNGLCREATEPROGRAMPROC_bd PFNGLCREATEPROGRAMPROC
#define PFNGLATTACHSHADERPROC_bd PFNGLATTACHSHADERPROC
#define PFNGLBINDATTRIBLOCATIONPROC_bd PFNGLBINDATTRIBLOCATIONPROC
#define PFNGLLINKPROGRAMPROC_bd PFNGLLINKPROGRAMPROC
#define PFNGLGETPROGRAMIVPROC_bd PFNGLGETPROGRAMIVPROC
#define PFNGLGETPROGRAMINFOLOGPROC_bd PFNGLGETPROGRAMINFOLOGPROC
#define PFNGLDELETEPROGRAMPROC_bd PFNGLDELETEPROGRAMPROC
#define PFNGLUSEPROGRAMPROC_bd PFNGLUSEPROGRAMPROC
#define PFNGLGETUNIFORMLOCATIONPROC_bd PFNGLGETUNIFORMLOCATIONPROC
#define PFNGLUNIFORM1IPROC_bd PFNGLUNIFORM1IPROC
#define PFNGLUNIFORM1FPROC_bd PFNGLUNIFORM1FPROC
#define PFNGLUNIFORM2FPROC_bd PFNGLUNIFORM2FPROC
#define PFNGLUNIFORM3FPROC_bd PFNGLUNIFORM3FPROC
#define PFNGLUNIFORM4FPROC_bd PFNGLUNIFORM4FPROC
#define PFNGLUNIFORMMATRIX4FVPROC_bd PFNGLUNIFORMMATRIX4FVPROC
#define PFNGLENABLEPROC_bd PFNGLENABLEPROC
#define PFNGLBLENDFUNCPROC_bd PFNGLBLENDFUNCPROC
#define PFNGLDISABLEPROC_bd PFNGLDISABLEPROC
#define PFNGLBUFFERDATAPROC_bd PFNGLBUFFERDATAPROC
#define PFNGLBUFFERSUBDATAPROC_bd PFNGLBUFFERSUBDATAPROC
#define PFNGLDRAWARRAYSPROC_bd PFNGLDRAWARRAYSPROC
#define PFNGLGENTEXTURESPROC_bd PFNGLGENTEXTURESPROC
#define PFNGLBINDTEXTUREPROC_bd PFNGLBINDTEXTUREPROC
#define PFNGLTEXIMAGE2DPROC_bd PFNGLTEXIMAGE2DPROC
#define PFNGLTEXPARAMETERIPROC_bd PFNGLTEXPARAMETERIPROC
#define PFNGLTEXSUBIMAGE2DPROC_bd PFNGLTEXSUBIMAGE2DPROC
#define PFNGLACTIVETEXTUREPROC_bd PFNGLACTIVETEXTUREPROC
#define PFNGLDELETETEXTURESPROC_bd PFNGLDELETETEXTURESPROC
#define PFNGLSCISSORPROC_bd PFNGLSCISSORPROC

PFNGLGENVERTEXARRAYSPROC_bd bd_glad_glGenVertexArrays;
PFNGLGENBUFFERSPROC_bd bd_glad_glGenBuffers;
PFNGLBINDVERTEXARRAYPROC_bd bd_glad_glBindVertexArray;
PFNGLBINDBUFFERPROC_bd bd_glad_glBindBuffer;
PFNGLENABLEVERTEXATTRIBARRAYPROC_bd bd_glad_glEnableVertexAttribArray;
PFNGLVERTEXATTRIBPOINTERPROC_bd bd_glad_glVertexAttribPointer;
PFNGLVIEWPORTPROC_bd bd_glad_glViewport;
PFNGLCLEARCOLORPROC_bd bd_glad_glClearColor;
PFNGLCLEARPROC_bd bd_glad_glClear;
PFNGLCREATESHADERPROC_bd bd_glad_glCreateShader;
PFNGLSHADERSOURCEPROC_bd bd_glad_glShaderSource;
PFNGLCOMPILESHADERPROC_bd bd_glad_glCompileShader;
PFNGLGETSHADERIVPROC_bd bd_glad_glGetShaderiv;
PFNGLGETSHADERINFOLOGPROC_bd bd_glad_glGetShaderInfoLog;
PFNGLDELETESHADERPROC_bd bd_glad_glDeleteShader;
PFNGLCREATEPROGRAMPROC_bd bd_glad_glCreateProgram;
PFNGLATTACHSHADERPROC_bd bd_glad_glAttachShader;
PFNGLBINDATTRIBLOCATIONPROC_bd bd_glad_glBindAttribLocation;
PFNGLLINKPROGRAMPROC_bd bd_glad_glLinkProgram;
PFNGLGETPROGRAMIVPROC_bd bd_glad_glGetProgramiv;
PFNGLGETPROGRAMINFOLOGPROC_bd bd_glad_glGetProgramInfoLog;
PFNGLDELETEPROGRAMPROC_bd bd_glad_glDeleteProgram;
PFNGLUSEPROGRAMPROC_bd bd_glad_glUseProgram;
PFNGLGETUNIFORMLOCATIONPROC_bd bd_glad_glGetUniformLocation;
PFNGLUNIFORM1IPROC_bd bd_glad_glUniform1i;
PFNGLUNIFORM1FPROC_bd bd_glad_glUniform1f;
PFNGLUNIFORM2FPROC_bd bd_glad_glUniform2f;
PFNGLUNIFORM3FPROC_bd bd_glad_glUniform3f;
PFNGLUNIFORM4FPROC_bd bd_glad_glUniform4f;
PFNGLUNIFORMMATRIX4FVPROC_bd bd_glad_glUniformMatrix4fv;
PFNGLENABLEPROC_bd bd_glad_glEnable;
PFNGLBLENDFUNCPROC_bd bd_glad_glBlendFunc;
PFNGLDISABLEPROC_bd bd_glad_glDisable;
PFNGLBUFFERDATAPROC_bd bd_glad_glBufferData;
PFNGLBUFFERSUBDATAPROC_bd bd_glad_glBufferSubData;
PFNGLDRAWARRAYSPROC_bd bd_glad_glDrawArrays;
PFNGLGENTEXTURESPROC_bd bd_glad_glGenTextures;
PFNGLBINDTEXTUREPROC_bd bd_glad_glBindTexture;
PFNGLTEXIMAGE2DPROC_bd bd_glad_glTexImage2D;
PFNGLTEXPARAMETERIPROC_bd bd_glad_glTexParameteri;
PFNGLTEXSUBIMAGE2DPROC_bd bd_glad_glTexSubImage2D;
PFNGLACTIVETEXTUREPROC_bd bd_glad_glActiveTexture;
PFNGLDELETETEXTURESPROC_bd bd_glad_glDeleteTextures;
PFNGLSCISSORPROC_bd bd_glad_glScissor;

#undef BD_GL_FUNC

/* Track whether the loader has run (to make it safe to call multiple times). */
static int bd_gl_loaded = 0;

/* Helper: load a single function pointer, with optional "required" flag.
 * Returns -1 if required but missing, 0 otherwise. */
static int
bd_gl_load_proc(void *(*getproc)(const char *name), const char *procname,
    void **out, int required)
{
	*out = getproc(procname);
	if (!*out && required) {
		fprintf(stderr, "bd_gl: missing required function: %s\n", procname);
		return -1;
	}
	return 0;
}

int
bd_gl_load(void *(*getproc)(const char *name))
{
	if (bd_gl_loaded)
		return 0;

	if (!getproc)
		return -1;

	/* Enumerate each required function. All are required (return -1 on first miss).
	 * bd_gl_loaded is set only after every pointer resolves, so a failed or
	 * NULL-getproc call leaves the loader retryable rather than silently
	 * "loaded" with NULL pointers. */
#define BD_GL_LOAD(name) \
	if (bd_gl_load_proc(getproc, "gl" #name, (void **)&bd_glad_gl##name, 1)) \
		return -1;

	BD_GL_LOAD(GenVertexArrays)
	BD_GL_LOAD(GenBuffers)
	BD_GL_LOAD(BindVertexArray)
	BD_GL_LOAD(BindBuffer)
	BD_GL_LOAD(EnableVertexAttribArray)
	BD_GL_LOAD(VertexAttribPointer)
	BD_GL_LOAD(Viewport)
	BD_GL_LOAD(ClearColor)
	BD_GL_LOAD(Clear)
	BD_GL_LOAD(CreateShader)
	BD_GL_LOAD(ShaderSource)
	BD_GL_LOAD(CompileShader)
	BD_GL_LOAD(GetShaderiv)
	BD_GL_LOAD(GetShaderInfoLog)
	BD_GL_LOAD(DeleteShader)
	BD_GL_LOAD(CreateProgram)
	BD_GL_LOAD(AttachShader)
	BD_GL_LOAD(BindAttribLocation)
	BD_GL_LOAD(LinkProgram)
	BD_GL_LOAD(GetProgramiv)
	BD_GL_LOAD(GetProgramInfoLog)
	BD_GL_LOAD(DeleteProgram)
	BD_GL_LOAD(UseProgram)
	BD_GL_LOAD(GetUniformLocation)
	BD_GL_LOAD(Uniform1i)
	BD_GL_LOAD(Uniform1f)
	BD_GL_LOAD(Uniform2f)
	BD_GL_LOAD(Uniform3f)
	BD_GL_LOAD(Uniform4f)
	BD_GL_LOAD(UniformMatrix4fv)
	BD_GL_LOAD(Enable)
	BD_GL_LOAD(BlendFunc)
	BD_GL_LOAD(Disable)
	BD_GL_LOAD(BufferData)
	BD_GL_LOAD(BufferSubData)
	BD_GL_LOAD(DrawArrays)
	BD_GL_LOAD(GenTextures)
	BD_GL_LOAD(BindTexture)
	BD_GL_LOAD(TexImage2D)
	BD_GL_LOAD(TexParameteri)
	BD_GL_LOAD(TexSubImage2D)
	BD_GL_LOAD(ActiveTexture)
	BD_GL_LOAD(DeleteTextures)
	BD_GL_LOAD(Scissor)

#undef BD_GL_LOAD

	bd_gl_loaded = 1;
	return 0;
}
