# birdie-gui on SDL3

A standalone example host that runs the **birdie-gui** toolkit on an
[SDL3](https://libsdl.org) window with an OpenGL ES 3 context, mixing the
toolkit's 2D UI with a hand-written 3D scene. It is a third reference backend
alongside ludica (`src/birdie/bd_backend_ludica.c`) and the raw X11/EGL/GLES
gallery (`src/guitest/`): the toolkit and renderer are untouched, only the
`bd_backend` GPU vtable and the native-event translation change.

The window shows a **rotatable 3D tetrahedron** drawn with raw GLES3 as the
background, and a birdie-gui UI composited on top: a **floating terminal
subwindow** you can drag by its title bar and **minimize**, and an **inventory
grid** (`bd_widget_inventory`) whose "Relic" cell shows the same spinning model.
Drag anywhere over the 3D background to rotate the tetrahedron; the mouse wheel
zooms; drag inventory items between slots.

The Relic cell demonstrates the **"host renders to a texture"** path a
backend-neutral widget relies on: each frame the example renders the spinning
tetrahedron into an offscreen framebuffer (`relic_init` / `relic_render`) and
hands that texture to the cell as its icon. The widget only ever blits
`item.icon`, so it never has to know about 3D; animation is just the host
updating the texture. (Y is flipped in the offscreen pass so the FBO image,
sampled top-left-origin by the 2D toolkit, comes out upright.)

Everything the backend needs lives in the single file `sdl3_example.c`:

- **GPU half** — raw GLES3 (shaders, a streaming VBO, textures, scissor). The
  toolkit's shaders are `#version 300 es`, so the example asks SDL for an
  OpenGL ES 3.0 context.
- **Window half** — SDL3 supplies the window, GL context, event pump, monotonic
  clock, clipboard, and IME. `SDL_Event`s are translated into the neutral
  `bd_event` the toolkit consumes.

A single window is enough, so the backend leaves `multi_window` at 0 and the
host presents the frame itself with `SDL_GL_SwapWindow()`.

## Compositing 2D over 3D

The toolkit renders its whole UI in one pass that normally begins by clearing
the framebuffer. Here the *host* owns the frame instead: each iteration it
clears, draws the 3D tetrahedron, then calls `bd_gui_render()`. So this backend
leaves `clear` NULL, which the toolkit treats as "the host clears the frame
itself" and skips. The 2D render state (blend on, depth off) is set in
`draw_verts`, not `clear`, so the UI still draws correctly over the 3D. The
toolkit's root frame is transparent (`BD_BG_C` alpha 0) so only the opaque
subwindow and text land on top of the tetrahedron.

The floating subwindow uses toolkit primitives directly: a `BD_LAYOUT_FIXED`
root positions the subwindow panel by `BD_X_I`/`BD_Y_I`, minimize toggles the
body's `BD_VISIBLE_B` and the panel height, and the host moves the window by
updating `BD_X_I`/`BD_Y_I` on an unconsumed title-bar drag. Left-drags the
toolkit does not consume (over the 3D background) rotate the tetrahedron.

## Build and run

The examples are a **separate modular-make project** (this directory has its own
copy of `GNUmakefile`), so the main birdie build never depends on SDL3. Build
from `examples/`, then run the binary **from the repo root** so the compiled-in
`BD_ASSET_*` font and atlas paths resolve against `src/birdie/assets/`:

```sh
cd examples && make                             # builds under examples/_out/
cd ..                                           # back to the repo root
examples/_out/x86_64-linux-gnu/bin/sdl3_example
```

Requires **SDL3** (found via `pkg-config sdl3`) and an OpenGL ES 3 loader
(`-lGLESv2`). The toolkit sources are pulled straight from `../src/birdie`, and
**libvt** (for the terminal widget) is built from `../src/libvt` by the same
project. Nothing is vendored twice.

## Porting notes

To host birdie-gui on any other windowing library, copy this file and replace:

- the `bd_backend` window functions (`be_width`/`be_height`/`be_time`) and the
  `SDL_GL_*` setup in `main()` with your library's equivalents, and
- `translate()` with your library's native-event → `bd_event` mapping.

The GLES3 GPU half is generic and can be kept as-is on any GLES-capable
context. For a plain UI app with no 3D background, provide a real `clear`
(`glClearColor` + `glClear`) and drop the host-side clear + 3D draw. See
`src/birdie/README.md` ("Porting to another backend") for the full `bd_backend`
contract.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)
