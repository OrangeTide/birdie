#ifndef BD_BACKEND_GLES_CORE_H
#define BD_BACKEND_GLES_CORE_H

/*
 * bd_backend_gles_core -- the GPU half of a bd_backend, implemented once
 * against raw OpenGL ES 3. Shaders, a streaming quad batch, textures, and
 * scissor are identical across every GLES host, so the SDL3 backend and the
 * raw X11/EGL/GLES gallery backend both bind their vtable rows to these
 * functions and supply only the windowing, input, clipboard, and IME that
 * genuinely differ.
 *
 * A backend still owns the GL context: it must create and make the context
 * current before calling any bd_gles_* function. It owns clear/scissor
 * *policy* too (whether to expose a clear hook, and what the framebuffer
 * height is), so scissor takes the height explicitly and clear is offered but
 * not forced.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_backend.h"

/*
 * Load OpenGL ES function pointers from a getproc callback. Required only if
 * birdie-gui was built with BD_GL_LOADER_BUILTIN (the default on Windows);
 * if BD_GL_LOADER_EXTERNAL, this is a no-op returning 0 (the application is
 * responsible for making glGenVertexArrays, glCreateShader, etc., available).
 *
 * Call once, after creating and making the GL context current, before any draw.
 * Returns 0 on success, -1 if any required function is missing.
 * Safe to call multiple times (second and later calls are no-ops).
 */
int        bd_gles_load_gl(void *(*getproc)(const char *name));

/* Set 2D UI viewport; thin wrapper over glViewport. */
void       bd_gles_viewport(int x, int y, int w, int h);

/* Clear the framebuffer to a color. A backend that owns the frame clear may
 * ignore this and leave its vtable clear NULL. */
void       bd_gles_clear(float r, float g, float b, float a);

/* Shaders: compile/link a program, bind/destroy, and set uniforms on the
 * active program. */
bd_shader  bd_gles_make_shader(const char *vert, const char *frag);
void       bd_gles_destroy_shader(bd_shader sh);
void       bd_gles_use_shader(bd_shader sh);
void       bd_gles_uniform_int  (bd_shader s, const char *n, int v);
void       bd_gles_uniform_float(bd_shader s, const char *n, float v);
void       bd_gles_uniform_vec2 (bd_shader s, const char *n, float x, float y);
void       bd_gles_uniform_vec3 (bd_shader s, const char *n, float x, float y, float z);
void       bd_gles_uniform_vec4 (bd_shader s, const char *n,
                                  float x, float y, float z, float w);
void       bd_gles_uniform_mat4 (bd_shader s, const char *n, const float m[16]);

/* Stream and draw a triangle list with the bound shader and texture unit 0.
 * Establishes the 2D UI render state (alpha blend, no depth or cull) itself. */
void       bd_gles_draw_verts(const bd_vertex *verts, int count);

/* Textures. load_* decode PNG via stb_image with NEAREST filtering (pixel-art
 * assets); make_texture defaults to LINEAR. The filter argument overrides. */
bd_texture bd_gles_load_texture(const char *path, bd_filter filter);
bd_texture bd_gles_load_texture_mem(const unsigned char *data, int len,
                                   bd_filter filter);
bd_texture bd_gles_make_texture(int w, int h, const void *rgba, bd_filter filter);
void       bd_gles_update_texture(bd_texture t, int x, int y, int w, int h,
                                  const void *rgba);
void       bd_gles_bind_texture(bd_texture t, int unit);
void       bd_gles_destroy_texture(bd_texture t);

/* Scissor. The toolkit clip is top-left pixels; GL's origin is bottom-left,
 * so the flip needs the framebuffer height of the window being rendered. The
 * caller passes it (that height is a windowing concern the core can't know). */
void       bd_gles_scissor(int x, int y, int w, int h, int fb_height);
void       bd_gles_scissor_off(void);

#endif
