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
	../src/birdie/bd_profile.c ../src/birdie/bd_vm.c ../src/birdie/bd_encoding.c \
	../src/birdie-gui/bd_utf8.c
# bd_mxp decodes numeric entities through bd_utf8 (in the toolkit dir).
test_client_CFLAGS = -I$(test_client_DIR)../src/birdie \
	-I$(test_client_DIR)../src/birdie-gui
test_client_LDLIBS = -lm

define test_client_TESTCMD
$(test_client_EXEC)
endef

# test_session is an integration test for bd_session, the seam that wires the
# transport to line assembly, triggers, and the front-end event stream. It links
# a FAKE bd_net (defined in the test) in place of the real socket/thread/TLS
# stack, so it drives the full bd_session.c pipeline deterministically with no
# network. bd_session_new creates its VM on the Lua backend; the test defines a
# no-op bd_vm_lua so the link needs no Lua/LPeg (command-body triggers, which the
# test exercises, work without a live interpreter).
EXECUTABLES  += test_session
TEST_TARGETS += test_session

test_session_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
test_session_SRCS  = test_session.c \
	../src/birdie/bd_session.c ../src/birdie/bd_log.c ../src/birdie/bd_replay.c \
	../src/birdie/bd_mxp.c ../src/birdie/bd_trigger.c ../src/birdie/bd_vm.c \
	../src/birdie/bd_profile.c ../src/birdie/bd_csv.c \
	../src/birdie-gui/bd_utf8.c
test_session_CFLAGS = -I$(test_session_DIR)../src/birdie \
	-I$(test_session_DIR)../src/birdie-gui
test_session_LDLIBS = -lm

define test_session_TESTCMD
$(test_session_EXEC)
endef

# test_netloop is a loopback integration test for the REAL bd_net: its net
# thread, the libiox poll loop, the rx/tx rings, and the telnet layer, driven
# against a local TCP socket the test plays the server side of. Unlike the other
# suites it links the vendored iox/mbedtls/miniz libraries bd_net needs and uses
# threads + sockets; every wait is timeout-bounded so a stuck socket fails a
# check rather than hanging. Plaintext only (no TLS handshake).
EXECUTABLES  += test_netloop
TEST_TARGETS += test_netloop

test_netloop_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
test_netloop_SRCS  = test_netloop.c \
	../src/birdie/bd_net.c ../src/birdie/bd_ring.c ../src/birdie/bd_telopt.c \
	../src/birdie/bd_encoding.c ../src/birdie-gui/bd_utf8.c
test_netloop_CFLAGS = -pthread -I$(test_netloop_DIR)../src/birdie \
	-I$(test_netloop_DIR)../src/birdie-gui
test_netloop_LIBS   = iox mbedtls miniz
test_netloop_LDLIBS = -pthread -lm

define test_netloop_TESTCMD
$(test_netloop_EXEC)
endef
