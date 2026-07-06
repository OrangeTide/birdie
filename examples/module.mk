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
# tree (../src/birdie-gui) and the terminal library from ../src/libvt, so
# nothing is vendored twice. Build outputs land in examples/_build and _out,
# isolated from the main project's.
#

PROJECT_CFLAGS   := -Wall -W
PROJECT_CXXFLAGS := -Wall -W

# ../src/libvt supplies the terminal widget's backing library (bd_vt); sdl3 and
# embed are the examples themselves.
SUBDIRS = ../src/libvt sdl3 embed
