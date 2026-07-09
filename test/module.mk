#
# test/module.mk — headless unit test for the birdie-gui toolkit.
#
# test_gui links the toolkit library (birdie_gui, which pulls in libvt) against
# a recording stub backend defined inside the test itself. No window, no ludica,
# no X11, so it runs in CI. Registered as a TEST_TARGET: `make run-test-test_gui`
# (or the top-level `make test` alias) builds it, runs it, and a failed check
# fails the build via the exit status.
#
# Made by a machine. PUBLIC DOMAIN (CC0-1.0)
#

EXECUTABLES  += test_gui
TEST_TARGETS += test_gui

test_gui_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
test_gui_SRCS  = test_gui.c
# birdie_gui exports its public-header dir and drags in bd_vt transitively; the
# test also reaches for the vendored stb single-headers via bd_draw.h.
test_gui_LIBS  = birdie_gui
test_gui_CPPFLAGS = -Isrc/thirdparty/stb
test_gui_LDLIBS = -lm

define test_gui_TESTCMD
$(test_gui_EXEC)
endef
