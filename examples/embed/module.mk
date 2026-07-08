#
# examples/embed/module.mk -- the birdie-gui embedded-assets example.
#
# Builds a self-contained binary: the chrome font, pushpin sprites, and terminal
# atlas are baked into the executable with .incbin (embed_assets.S) and served
# through the bd_asset registry, so the program reads no asset files at runtime.
# It runs on the raw X11/EGL/GLES backend (src/guitest), the same one the widget
# gallery uses, so it needs no SDL3 or ludica -- only X11 + EGL + GLESv2.
#
# Made by a machine. PUBLIC DOMAIN (CC0-1.0)
#

EXECUTABLES += embed_example

embed_example_DIR := $(dir $(lastword $(MAKEFILE_LIST)))

# Path to the toolkit's stock assets, for .incbin only. Kept RELATIVE on purpose:
# an $(abspath ...) here would put the build machine's directory layout into the
# assembler command line. The .incbin path never reaches the binary (it copies
# file *contents*), but keeping it relative means the build itself is clean and
# reproducible. The examples build always runs from examples/, where this points
# at the repo's assets.
embed_ASSETS := $(embed_example_DIR)../../src/birdie-gui/assets

# Toolkit sources this example actually uses: the widget core, the renderer, the
# embedded-asset registry (bd_asset.c -- the piece that makes this work), the
# terminal widget, and the rich-text editor (which renders the embedded
# monospace family). The GLES backend + X11 window come from src/guitest.
embed_example_TOOLKIT := \
    ../../src/birdie-gui/widget.c ../../src/birdie-gui/bd_draw.c \
    ../../src/birdie-gui/bd_asset.c ../../src/birdie-gui/bd_widget_vt.c \
    ../../src/birdie-gui/bd_widget_editor.c
embed_example_BACKEND := \
    ../../src/guitest/bd_backend_gles.c ../../src/birdie-gui/bd_backend_gles_core.c \
    ../../src/guitest/x11_window.c

embed_example_SRCS = embed_example.c embed_assets.S \
    $(embed_example_TOOLKIT) $(embed_example_BACKEND)
embed_example_LIBS = bd_vt
# The .incbin source paths (build-time only; never end up in the binary).
#
# NOTE: this example deliberately does NOT override BD_ASSET_* to basenames.
# It shares the compiled toolkit objects (widget.o, bd_draw.o, ...) with the
# SDL3 example in this same modular-make project, so it cannot compile them with
# its own -D flags without corrupting the other example. The upstream RELATIVE
# default paths ("src/birdie-gui/assets/...") therefore remain in the binary;
# they are the same for every build and expose nothing about the build machine.
# A real consumer compiling birdie-gui as its own single executable overrides
# BD_ASSET_* to bare basenames and bakes in no path at all -- see this example's
# README and doc/gui.md. What matters here is that no ABSOLUTE build path leaks
# (embed_ASSETS is relative, and .incbin paths never reach the binary).
embed_example_CPPFLAGS = \
    -I$(embed_example_DIR)../../src/birdie-gui \
    -I$(embed_example_DIR)../../src/guitest \
    -I$(embed_example_DIR)../../src/thirdparty/stb \
    -DEMBED_FONT_UI='"$(embed_ASSETS)/fonts/DejaVuSans.ttf"' \
    -DEMBED_PIN_OUT='"$(embed_ASSETS)/pushpin/pushpin-out-14.png"' \
    -DEMBED_PIN_IN='"$(embed_ASSETS)/pushpin/pushpin-in-14.png"' \
    -DEMBED_TERM_ATLAS='"$(embed_ASSETS)/font8x16.png"' \
    -DEMBED_FONT_MONO='"$(embed_ASSETS)/fonts/DejaVuSansMono.ttf"' \
    -DEMBED_FONT_MONO_BOLD='"$(embed_ASSETS)/fonts/DejaVuSansMono-Bold.ttf"' \
    -DEMBED_FONT_MONO_ITALIC='"$(embed_ASSETS)/fonts/DejaVuSansMono-Oblique.ttf"'
embed_example_LDLIBS = -lX11 -lXi -lEGL -lGLESv2 -lm
