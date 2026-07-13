#
# test/module.mk -- headless unit test for the birdie-gui toolkit.
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
# The test exercises the terminal widget, so it links birdie_gui_vt, which drags
# in birdie_gui (its public-header dir + bd_utf8) transitively.
test_gui_LIBS  = birdie_gui_vt birdie_gui
test_gui_LDLIBS = -lm

define test_gui_TESTCMD
$(test_gui_EXEC)
endef

# test_client exercises the MUD-client core (bd_ring/csv/telopt/trigger/profile).
# Those units are socket-free, callback-driven state machines, so the test
# compiles them directly (no ludica, no sockets, no threads) and needs no
# external libraries. bd_trigger references bd_vm_eval for '@' Lua bodies, so
# bd_vm.c (its null backend) is linked to satisfy the symbol; the tests use only
# command bodies, so no scripting runtime is required.
EXECUTABLES  += test_client
TEST_TARGETS += test_client

test_client_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
test_client_SRCS  = test_client.c \
	../src/birdie/bd_ring.c ../src/birdie/bd_csv.c ../src/birdie/bd_telopt.c \
	../src/birdie/bd_trigger.c ../src/birdie/bd_verb.c ../src/birdie/bd_mxp.c \
	../src/birdie/bd_profile.c ../src/birdie/bd_vm.c \
	../src/birdie-gui/bd_utf8.c
# bd_mxp decodes numeric entities through bd_utf8 (in the toolkit dir).
test_client_CFLAGS = -I$(test_client_DIR)../src/birdie \
	-I$(test_client_DIR)../src/birdie-gui
test_client_LDLIBS = -lm

define test_client_TESTCMD
$(test_client_EXEC)
endef
