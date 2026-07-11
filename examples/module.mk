#
# examples/ top-level module.mk
#
# The examples are a *separate* modular-make project so the main birdie build
# never depends on SDL3 (or any other example-only library). This directory
# carries its own copy of GNUmakefile; build the examples by running make from
# here:
#
#   cd examples && make          # builds every example under _out/
#
# Each example pulls the birdie-gui toolkit sources straight from the parent
# tree (../src/birdie-gui), including the terminal engine under
# ../src/birdie-gui/bd_vt, so nothing is vendored twice. Build outputs land in
# examples/_build and _out, isolated from the main project's.
#

PROJECT_CFLAGS   := -Wall -W
PROJECT_CXXFLAGS := -Wall -W

# The examples compile the toolkit (and VT) sources directly, so there are no
# library subdirs to pull in; sdl3, canvas, and embed are the examples
# themselves. canvas is the minimal passthrough-canvas demo, sdl3 the full one.
SUBDIRS = sdl3 canvas embed

# Stage the toolkit's fonts next to the example binaries, so the SDL3 example
# (which loads fonts from disk) finds them via the backend's resolve_asset hook
# regardless of the working directory. The embed example bakes its own fonts in
# and needs none of these. Sources live in the parent tree; the toolkit names
# them only by their asset-root-relative sub-path. (Pushpins are compiled into
# the toolkit, no asset file.)
BD_ASSET_DIR := ../src/birdie-gui/assets
BD_FONT_OUT  := $(patsubst $(BD_ASSET_DIR)/fonts/%,$(BINDIR)/fonts/%,\
                  $(wildcard $(BD_ASSET_DIR)/fonts/*.ttf))

$(BINDIR)/fonts/% : $(BD_ASSET_DIR)/fonts/% | $(BINDIR)/fonts
	cp $< $@
$(BINDIR)/fonts :
	mkdir -p $@

all :: $(BD_FONT_OUT)
