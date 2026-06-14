#
# birdie top-level module.mk
#
# Pulls in the vendored ludica tree (src/thirdparty/ludica/) plus birdie's
# own sources under src/birdie/. We deliberately do *not* include ludica's
# own src/module.mk — its SUBDIRS pull in samples/imgui/tools which we
# don't need, and its shader rule assumes `tools/glsl2h` lives at the
# make-invocation root. Instead we SUBDIR into just the ludica library
# dirs we care about and redeclare the shader rule with the vendored path.
#

PROJECT_CFLAGS   := -Wall -W
PROJECT_CXXFLAGS := -Wall -W

LUDICA := src/thirdparty/ludica

SUBDIRS = \
	$(LUDICA)/src/thirdparty \
	$(LUDICA)/src/libiox \
	$(LUDICA)/src/ludica \
	$(LUDICA)/tools \
	src/libvt \
	src/birdie

# Shader-to-C generation using the vendored glsl2h.
$(BUILDDIR)/%.c : %.glsl $(LUDICA)/tools/glsl2h
	$(LUDICA)/tools/glsl2h $< > $@

# ----------------------------------------------------------------------
# Source distribution of the GUI toolkit.
#
# `make dist` bundles the toolkit's public API, implementation, reference
# ludica backend, VT extension, and runtime assets into a versioned ZIP
# under $(OUTDIR). Override the version with `make dist GUI_VERSION=x.y.z`.
# The bundle is source-only; consumers supply ludica (reference backend +
# GUI font) and libvt (terminal widget) themselves.
# ----------------------------------------------------------------------
GUI_VERSION ?= 0.2.0
DIST_NAME   := birdie-gui-$(GUI_VERSION)
DIST_STAGE  := $(OUTDIR)/$(DIST_NAME)
DIST_ZIP    := $(OUTDIR)/$(DIST_NAME).zip
DIST_SRC    := src/birdie

DIST_HEADERS := widget.h widget_ext.h bd_backend.h bd_theme.h bd_widget_vt.h
DIST_SOURCES := widget.c bd_widget_vt.c bd_backend_ludica.c bd_backend_ludica.h

.PHONY : dist
dist :
	@rm -rf $(DIST_STAGE) $(DIST_ZIP)
	@mkdir -p $(DIST_STAGE)/include $(DIST_STAGE)/src $(DIST_STAGE)/assets/pushpin
	@cp $(addprefix $(DIST_SRC)/,$(DIST_HEADERS)) $(DIST_STAGE)/include/
	@cp $(addprefix $(DIST_SRC)/,$(DIST_SOURCES)) $(DIST_STAGE)/src/
	@cp $(DIST_SRC)/assets/font8x16.png $(DIST_STAGE)/assets/
	@cp $(DIST_SRC)/assets/pushpin/pushpin-out-14.png \
	    $(DIST_SRC)/assets/pushpin/pushpin-in-14.png $(DIST_STAGE)/assets/pushpin/
	@cp $(DIST_SRC)/README.md $(DIST_STAGE)/
	@printf '%s\n' \
	    'birdie-gui $(GUI_VERSION)' \
	    '' \
	    'Portable retained-mode GUI toolkit (C). Source distribution.' \
	    'See README.md for usage; include/ has the public API headers and' \
	    'src/ the implementation plus the reference ludica backend.' \
	    '' \
	    'External dependencies (provide these yourself):' \
	    '  ludica  reference backend src/bd_backend_ludica.c; also ships the' \
	    '          GUI vector font. For SDL/raylib/GLFW, implement bd_backend.' \
	    '  libvt   terminal widget src/bd_widget_vt.c.' \
	    '' \
	    'Made by a machine. PUBLIC DOMAIN (CC0-1.0)' \
	    > $(DIST_STAGE)/MANIFEST.txt
	@cd $(OUTDIR) && zip -rq $(DIST_NAME).zip $(DIST_NAME)
	@echo "dist: wrote $(DIST_ZIP)"

# ----------------------------------------------------------------------
# Headless unit test for the GUI toolkit (test/test_gui.c). Compiles the
# toolkit sources together with a recording stub backend and runs them
# with no window, no ludica, and no X11, so it works in CI. Links only
# libvt. Exit status propagates, so a failing check fails the build.
# ----------------------------------------------------------------------
TEST_BIN := $(BUILDDIR)/test_gui

.PHONY : test
test : bd_vt
	@mkdir -p $(BUILDDIR)
	cc -Wall -W -Isrc/birdie -Isrc/libvt -Isrc/thirdparty/stb \
	    test/test_gui.c src/birdie/widget.c src/birdie/bd_widget_vt.c \
	    src/birdie/bd_draw.c src/birdie/bd_widget_value.c \
	    src/birdie/bd_widget_explorer.c \
	    $(BUILDDIR)/bd_vt.a -lm -o $(TEST_BIN)
	@echo "running headless GUI test:"
	@$(TEST_BIN)

# ----------------------------------------------------------------------
# Widget gallery (src/guitest/). A standalone windowed sample on the raw
# OpenGL ES 3 backend (src/guitest/bd_backend_gles.c + x11_window.c),
# independent of ludica, that exhibits and exercises every working widget.
# birdie runs on ludica, this runs on GLES, so both backends stay tested.
# Linux/X11 only and opt-in (not built by `all`); run from the repo root so
# the default BD_ASSET_* paths resolve. Links libvt for the terminal widget.
# ----------------------------------------------------------------------
GALLERY_BIN := $(BUILDDIR)/birdie-gui-gallery

.PHONY : widget-test
widget-test : bd_vt
	@mkdir -p $(BUILDDIR)
	cc -Wall -W -Isrc/birdie -Isrc/guitest -Isrc/libvt -Isrc/thirdparty/stb \
	    src/guitest/widget_test.c src/guitest/x11_window.c \
	    src/guitest/bd_backend_gles.c \
	    src/birdie/widget.c src/birdie/bd_widget_vt.c \
	    src/birdie/bd_draw.c src/birdie/bd_widget_value.c \
	    $(BUILDDIR)/bd_vt.a \
	    -lX11 -lEGL -lGLESv2 -lm -o $(GALLERY_BIN)
	@echo "built widget gallery: $(GALLERY_BIN)"
	@echo "run it from the repo root: $(GALLERY_BIN)"
