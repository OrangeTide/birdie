#
# examples/canvas/module.mk -- the minimal birdie-gui passthrough-canvas example.
#
# The smallest host for the v0.8.1 GLES-background canvas: a spinning triangle
# drawn into a passthrough BD_MANAGED_CANVAS, with the toolkit composited on top
# and input routed through the canvas callbacks. It shares the SDL3 backend with
# sdl3_example but pulls in far fewer toolkit sources (no terminal, editor, or
# extra widgets), so it reads as a focused API demonstration.
#

EXECUTABLES += canvas_example

canvas_example_DIR := $(dir $(lastword $(MAKEFILE_LIST)))

# Just the toolkit core, the SDL3 + GLES backend, and the inventory (drag
# source) plus its shared icon cell.
canvas_example_TOOLKIT := \
    ../../src/birdie-gui/widget.c ../../src/birdie-gui/bd_draw.c \
    ../../src/birdie-gui/bd_asset.c ../../src/birdie-gui/bd_utf8.c \
    ../../src/birdie-gui/bd_color.c \
    ../../src/birdie-gui/bd_backend_sdl3.c \
    ../../src/birdie-gui/bd_backend_gles_core.c \
    ../../src/birdie-gui/bd_widget_inventory.c ../../src/birdie-gui/bd_widget_icon.c

canvas_example_SRCS  = canvas_example.c $(canvas_example_TOOLKIT)
canvas_example_CPPFLAGS = -I$(canvas_example_DIR)../../src/birdie-gui \
                          -I$(canvas_example_DIR)../../src/birdie-gui/thirdparty/stb \
                          $(shell pkg-config --cflags sdl3)
canvas_example_LDLIBS = $(shell pkg-config --libs sdl3) -lGLESv2 -lm
