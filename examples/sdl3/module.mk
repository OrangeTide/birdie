#
# examples/sdl3/module.mk — the birdie-gui SDL3 example.
#
# Compiles the demo host (sdl3_example.c) together with the SDL3 backend and the
# birdie-gui toolkit sources from the parent tree, and links libvt (bd_vt) for
# the terminal widget. SDL3 is resolved through pkg-config, so this example only
# builds where SDL3 is installed; the main birdie project never needs it.
#

EXECUTABLES += sdl3_example

sdl3_example_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))

# Toolkit sources live in the main tree (../src/birdie-gui). This mirrors the
# set the widget gallery compiles, so any widget the example grows into keeps
# linking. bd_backend_sdl3.c is the SDL3 backend, kept beside the ludica one.
sdl3_example_TOOLKIT := \
    ../../src/birdie-gui/widget.c ../../src/birdie-gui/bd_draw.c \
    ../../src/birdie-gui/bd_backend_sdl3.c \
    ../../src/birdie-gui/bd_widget_vt.c ../../src/birdie-gui/bd_widget_value.c \
    ../../src/birdie-gui/bd_widget_explorer.c ../../src/birdie-gui/bd_widget_editor.c \
    ../../src/birdie-gui/bd_widget_canvas.c ../../src/birdie-gui/bd_widget_table.c \
    ../../src/birdie-gui/bd_widget_inventory.c

sdl3_example_SRCS  = sdl3_example.c $(sdl3_example_TOOLKIT)
sdl3_example_LIBS  = bd_vt
sdl3_example_CPPFLAGS = -I$(sdl3_example_DIR)../../src/birdie-gui \
                        -I$(sdl3_example_DIR)../../src/thirdparty/stb \
                        $(shell pkg-config --cflags sdl3)
sdl3_example_LDLIBS = $(shell pkg-config --libs sdl3) -lGLESv2 -lm
