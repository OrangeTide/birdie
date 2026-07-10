# OpenGL ES Function Loader

birdie-gui's GLES rendering backend can run on Windows and Linux through an optional built-in GL function loader, decoupling symbol resolution from link-time to run-time. This solves a Windows portability issue: `opengl32.dll` exports only OpenGL 1.1, so ES 3.0 entry points must be loaded at runtime.

## Overview

The toolkit supports two modes:

1. **Built-in loader (default: `BD_GL_LOADER=builtin`)** — birdie-gui provides function pointers and a loader that resolves them from a caller-supplied `getproc` callback (e.g., `SDL_GL_GetProcAddress`, `eglGetProcAddress`, or a custom shim). Call `bd_gles_load_gl(getproc)` once after creating and making the GL context current, before any draw.

2. **External loader (`BD_GL_LOADER=external`)** — The application is responsible for making `glGenVertexArrays`, `glCreateShader`, etc., available (GLEW, GLAD, Galogen, direct linking, or a custom loader). `bd_gles_load_gl()` is a no-op returning success (so backends can call it unconditionally).

## Implementation

### Files Added

- `src/birdie-gui/bd_gl.h` — Loader function declaration.
- `src/birdie-gui/bd_gl.c` — Loader implementation (function pointer storage + init).
- `src/birdie-gui/GLES3/gl3.h` — Shim header (builtin mode: redirects GL calls to function pointers; external mode: pass-through to real Khronos header).

### Backend Changes

- `src/birdie-gui/bd_backend_gles_core.h` — Declares `bd_gles_load_gl()` for public use.
- `src/birdie-gui/bd_backend_gles_core.c` — Implements `bd_gles_load_gl()` (dispatches to `bd_gl_load()` in builtin mode, no-op in external mode).
- `src/birdie-gui/bd_backend_sdl3.c` — Calls `bd_gles_load_gl(SDL_GL_GetProcAddress)` after creating the GL context.
- `src/guitest/widget_test.c` — Calls `bd_gles_load_gl(eglGetProcAddress)` after window init.

### Build System

- `module.mk` (top-level) — Defines `BD_GL_LOADER` (builtin/external; default builtin). Conditionally compiles `bd_gl.c` and adds the shim include dir in builtin mode.

## GL Functions Covered

The loader handles all GL ES 3.0 entry points used by the toolkit (44 functions):

Vertex Array / Buffer: `glGenVertexArrays`, `glBindVertexArray`, `glGenBuffers`, `glBindBuffer`, `glEnableVertexAttribArray`, `glVertexAttribPointer`, `glBufferData`, `glBufferSubData`, `glDrawArrays`

Shader: `glCreateShader`, `glShaderSource`, `glCompileShader`, `glGetShaderiv`, `glGetShaderInfoLog`, `glDeleteShader`, `glCreateProgram`, `glAttachShader`, `glBindAttribLocation`, `glLinkProgram`, `glGetProgramiv`, `glGetProgramInfoLog`, `glDeleteProgram`, `glUseProgram`, `glGetUniformLocation`

Uniforms: `glUniform1i`, `glUniform1f`, `glUniform2f`, `glUniform3f`, `glUniform4f`, `glUniformMatrix4fv`

Texture: `glGenTextures`, `glBindTexture`, `glTexImage2D`, `glTexParameteri`, `glTexSubImage2D`, `glActiveTexture`, `glDeleteTextures`

Render state: `glViewport`, `glClearColor`, `glClear`, `glEnable`, `glBlendFunc`, `glDisable`, `glScissor`

## Usage

### Builtin Mode (Default, Windows-Safe)

```c
#include "bd_backend_sdl3.h"  // or bd_backend_gles.h

SDL_Window *win = bd_backend_sdl3_open("title", 800, 600);
if (!win) return 1;

/* SDL3 backend calls bd_gles_load_gl internally; if you're writing a custom
 * backend, call it explicitly after making the GL context current. */

bd_gui_init(&bd_backend_sdl3, NULL);
```

The SDL3 backend (`bd_backend_sdl3_open`) calls `bd_gles_load_gl(SDL_GL_GetProcAddress)` automatically. If using the raw GLES backend or a custom host, call it yourself:

```c
#include "bd_backend_gles_core.h"

/* SDL_GL_GetProcAddress returns an SDL_FunctionPointer (void (*)(void)),
 * so cast it to the getproc type; eglGetProcAddress needs the same cast. */
if (bd_gles_load_gl((void *(*)(const char *))SDL_GL_GetProcAddress) != 0) {
    fprintf(stderr, "GL loader failed\n");
    return 1;
}
```

### External Mode

```bash
make BD_GL_LOADER=external widget-test
```

The shim header passes through to the real Khronos header; the application must make GL symbols available (e.g., by linking `-lGLESv2` on Linux, or calling `glewInit()` after context creation if using GLEW).

```c
/* Example: GLEW */
glewInit();
bd_gui_init(&bd_backend_sdl3, NULL);  /* no loader call needed */

/* Example: Direct linking on Linux with libGLESv2 */
cc -o myapp myapp.c ... -lGLESv2
```

## Build Flags

```bash
make BD_GL_LOADER=builtin  widget-test   # Default (all platforms)
make BD_GL_LOADER=external widget-test   # External loader mode
```

In `CPPFLAGS`:
- Builtin: `-DBD_GL_LOADER_BUILTIN -I./src/birdie-gui` (adds shim include path)
- External: `-DBD_GL_LOADER_EXTERNAL`

## Compatibility

- **Linux**: Builtin mode works with EGL (`eglGetProcAddress`); external mode works with libGLESv2 link-time symbols.
- **Windows**: Builtin mode (recommended) works with any ES 3.0 loader (GLEW, GLAD, Galogen, or custom). External mode requires linking static GLES libs or manual symbol provision (non-standard).
- **macOS**: Builtin mode works with any getproc the host provides (e.g. `SDL_GL_GetProcAddress`, or a custom loader over the OpenGL framework); external mode requires system framework linking. (macOS GL is deprecated and only a stretch-goal target.)

## Vendored Khronos headers

The built-in loader's `GLES3/gl3.h` shim needs a real Khronos `GLES3/gl3.h`
underneath (via `#include_next`) for the GL types, enums, and `PFNGL*PROC`
typedefs. Windows/mingw ships `KHR/khrplatform.h` but no `GLES3/gl3.h`, so the
Khronos ES 3.0 headers are vendored under `src/birdie-gui/thirdparty/khronos/`
(MIT / Apache-2.0; see its `UPSTREAM`), refreshed with
`scripts/update-khronos.sh` (`gl3.h` + `gl3platform.h` from the OpenGL registry,
`khrplatform.h` from the EGL registry). The builtin build adds that directory to
the include path after the shim dir, so the shim resolves to it on every target.
On Linux these mirror the system headers; only the core `gl3.h` is vendored (the
backend uses no extensions).

## Verifying the Windows target

`make windows-check` cross-compiles the GUI libraries with mingw-w64 as a
compile+archive check of the Windows target (no `.exe`, no GL runtime, since the
loader resolves `gl*` at runtime). It runs in CI (`.github/workflows/ci.yml`)
and needs only the toolchain, no X11/GL dev libs. This is the honest validation
of the loader's reason to exist; the Linux gallery is the runtime exercise of the
loaded pointers.

## Contract for Vendorers

When vendoring birdie-gui:

1. If you want the portable built-in loader (recommended for Windows), use builtin mode (default):
   ```makefile
   your_app_LIBS = birdie_gui_gles_core birdie_gui
   # bd_gl.c compiles automatically; call bd_gles_load_gl(your_getproc) after context init.
   ```

2. If you already have a GL loader (GLEW, GLAD, Galogen), use external mode by
   passing `BD_GL_LOADER=external` on the make command line (the build turns it
   into `-DBD_GL_LOADER_EXTERNAL`; do not set that flag by hand):
   ```makefile
   # make BD_GL_LOADER=external ...
   your_app_LIBS = birdie_gui_gles_core birdie_gui
   # Shim passes through; your loader makes gl* available before toolkit init.
   ```

3. If neither, consider adopting the loader to simplify Windows deployment.
