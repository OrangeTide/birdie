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
# It links birdie_client (the shared client library) and pulls only the units it
# references from the archive; the socket/thread/TLS stack (bd_net) stays
# unpulled because the test never references it. bd_trigger references bd_vm_eval
# for '@' Lua bodies, resolved by birdie_client's null bd_vm backend; the tests
# use only command bodies, so no scripting runtime is required. bd_utf8 (used by
# bd_mxp) comes transitively from birdie_gui via birdie_client.
EXECUTABLES  += test_client
TEST_TARGETS += test_client

test_client_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
test_client_SRCS  = test_client.c
test_client_LIBS  = birdie_client
test_client_LDLIBS = -lm

define test_client_TESTCMD
$(test_client_EXEC)
endef

# test_fs exercises bd_fs, the file-dialog filesystem model. The model is
# UI-agnostic and its OS access sits behind a platform vtable, so the test
# compiles bd_fs.c directly (no toolkit, no backend, no window) and drives it
# through a fake platform plus one real POSIX scandir against a temp directory.
EXECUTABLES  += test_fs
TEST_TARGETS += test_fs

# bd_fs lives in the birdie_gui toolkit; link it so the object is compiled once.
# Static-archive linking pulls only bd_fs.o (+ bd_utf8.o it uses), not the
# widgets or any backend. The test drives the real bd_fs_platform_default() and
# injects its fakes through the runtime vtable, so no symbol override is needed.
test_fs_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
test_fs_SRCS  = test_fs.c
test_fs_LIBS  = birdie_gui

define test_fs_TESTCMD
$(test_fs_EXEC)
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

# Links birdie_client and defines its own fake bd_net + no-op bd_vm_lua. Those
# fakes satisfy bd_session's references first, so the archive's real bd_net.o
# (and its iox/mbedtls/miniz deps) is never pulled and no Lua runtime is needed.
test_session_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
test_session_SRCS  = test_session.c
test_session_LIBS  = birdie_client
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

# Links birdie_client for the REAL bd_net (and the ring/telopt/encoding it
# pulls); iox/mbedtls/miniz come transitively. The test's own TU needs -pthread
# and links mbedtls directly for its in-test TLS server (already via
# birdie_client). No fakes here, so bd_net.o is pulled and exercised.
test_netloop_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
test_netloop_SRCS  = test_netloop.c
test_netloop_LIBS   = birdie_client
test_netloop_CFLAGS = -pthread
test_netloop_LDLIBS = -pthread -lm

define test_netloop_TESTCMD
$(test_netloop_EXEC)
endef

# test_mcp exercises bd_mcp, the MUD Client Protocol 2.1 core. bd_mcp is a
# self-contained, callback-driven state machine (no session, net, or UI), so the
# test compiles it directly and drives the handshake, negotiation, quoting, and
# the simpleedit round trip with recording callbacks. No external libraries.
EXECUTABLES  += test_mcp
TEST_TARGETS += test_mcp

# Links birdie_client and pulls only bd_mcp.o (self-contained; no session, net,
# or UI), so nothing else from the archive is dragged in.
test_mcp_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
test_mcp_SRCS  = test_mcp.c
test_mcp_LIBS  = birdie_client

define test_mcp_TESTCMD
$(test_mcp_EXEC)
endef
