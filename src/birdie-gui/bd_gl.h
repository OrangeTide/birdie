/*
 * bd_gl.h -- OpenGL ES 3.0 function pointers and loader.
 *
 * When BD_GL_LOADER_BUILTIN is defined (the default), this module supplies
 * function pointers for all GLES functions birdie-gui uses. Call bd_gl_load()
 * once after creating and making the GL context current; it resolves all
 * function addresses via a caller-supplied getproc function (e.g.
 * SDL_GL_GetProcAddress, eglGetProcAddress, or a custom shim).
 *
 * When BD_GL_LOADER_EXTERNAL is defined, this header is a no-op: the
 * application is responsible for making glGenVertexArrays, glCreateShader,
 * etc., available in the current address space (via GLEW, GLAD, Galogen,
 * a custom loader, or direct linking on systems that export them).
 * bd_gl_load() becomes a no-op returning success (so call sites are
 * unconditional).
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#ifndef BD_GL_H
#define BD_GL_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Load all GLES function pointers via a getproc callback.
 * Returns 0 on success, -1 if any required entry point is missing.
 * Safe to call multiple times (second and later calls are no-ops).
 */
int bd_gl_load(void *(*getproc)(const char *name));

#ifdef __cplusplus
}
#endif

#endif
