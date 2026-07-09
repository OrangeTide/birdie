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
# library subdirs to pull in; sdl3 and embed are the examples themselves.
SUBDIRS = sdl3 embed
