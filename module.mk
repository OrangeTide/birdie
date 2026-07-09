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
	src/thirdparty/mbedtls \
	src/thirdparty/miniz \
	src/thirdparty/lua \
	src/thirdparty/lpeg \
	src/libvt \
	src/birdie-gui \
	src/birdie \
	test

# The widget gallery (src/guitest) needs X11/EGL/GLES and is Linux-only, so it
# is opt-in: its module.mk is loaded only when WIDGET_TEST is set, keeping the
# gallery out of the default `all`. The `widget-test` target below re-enters
# make with the flag set.
ifdef WIDGET_TEST
SUBDIRS += src/guitest
endif

# Shader-to-C generation using the vendored glsl2h.
$(BUILDDIR)/%.c : %.glsl $(LUDICA)/tools/glsl2h
	$(LUDICA)/tools/glsl2h $< > $@

# ----------------------------------------------------------------------
# Convenience aliases for the modular-make targets declared in the
# module.mk files above, so the documented entry points keep working:
#
#   make test         build + run the headless toolkit test (test/module.mk).
#   make widget-test   build the opt-in widget gallery (src/guitest/module.mk).
# ----------------------------------------------------------------------
.PHONY : test
test : run-test-test_gui

.PHONY : widget-test
widget-test :
	@$(MAKE) WIDGET_TEST=1 birdie-gui-gallery
	@echo "built widget gallery: $(BINDIR)/birdie-gui-gallery"
	@echo "run it from the repo root: $(BINDIR)/birdie-gui-gallery"

# ----------------------------------------------------------------------
# Source distribution of the GUI toolkit (birdie-gui).
#
# `make dist` bundles the toolkit's public API, implementation, three reference
# backends (ludica, SDL3, and raw X11/EGL/GLES), all extension widgets, the
# standalone widget gallery, vendored libvt (so the terminal widget compiles),
# the vendored stb single-headers, and runtime assets into a versioned ZIP
# under $(OUTDIR). Override the version with `make dist GUI_VERSION=x.y.z`.
# Each backend still needs its own host library (ludica / SDL3 / X11+EGL).
# ----------------------------------------------------------------------
GUI_VERSION ?= 0.6.0
DIST_NAME   := birdie-gui-$(GUI_VERSION)
DIST_STAGE  := $(OUTDIR)/$(DIST_NAME)
DIST_ZIP    := $(OUTDIR)/$(DIST_NAME).zip
DIST_SRC    := src/birdie-gui
DIST_GLES   := src/guitest
DIST_STB    := src/thirdparty/stb

# public API headers
DIST_HEADERS := widget.h widget_ext.h bd_backend.h bd_theme.h bd_draw.h \
                bd_asset.h bd_backend_gles_core.h \
                bd_widget_vt.h bd_widget_value.h bd_widget_explorer.h \
                bd_widget_editor.h bd_widget_canvas.h bd_widget_table.h \
                bd_widget_inventory.h bd_widget_dock.h bd_widget_actionbar.h \
                bd_widget_tabview.h bd_widget_indicator.h
# toolkit implementation + reference ludica and SDL3 backends. The shared GLES
# GPU core (bd_backend_gles_core.c) backs both the SDL3 and X11/EGL/GLES
# backends; its header ships in include/ so either resolves it with -Iinclude.
DIST_SOURCES := widget.c bd_draw.c bd_fallback_font.h bd_asset.c bd_widget_vt.c bd_widget_value.c \
                bd_widget_explorer.c bd_widget_editor.c bd_widget_canvas.c \
                bd_widget_table.c bd_widget_inventory.c bd_widget_dock.c \
                bd_widget_actionbar.c bd_widget_tabview.c \
                bd_widget_indicator.c \
                bd_backend_gles_core.c \
                bd_backend_ludica.c bd_backend_ludica.h \
                bd_backend_sdl3.c bd_backend_sdl3.h
# raw X11/EGL/GLES reference backend + widget gallery (Linux)
DIST_GLES_FILES := window.h x11_window.c bd_backend_gles.c bd_backend_gles.h \
                   widget_test.c
# vendored single-file libraries the toolkit includes
DIST_STB_FILES := stb_truetype.h stb_image.h
# libvt (terminal escape-sequence engine) — bundled so the VT widget compiles
DIST_LIBVT  := src/libvt

.PHONY : dist
dist :
	@rm -rf $(DIST_STAGE) $(DIST_ZIP)
	@mkdir -p $(DIST_STAGE)/include $(DIST_STAGE)/src \
	    $(DIST_STAGE)/backend-gles $(DIST_STAGE)/libvt \
	    $(DIST_STAGE)/thirdparty/stb \
	    $(DIST_STAGE)/assets/fonts $(DIST_STAGE)/assets/pushpin
	@cp $(addprefix $(DIST_SRC)/,$(DIST_HEADERS)) $(DIST_STAGE)/include/
	@cp $(addprefix $(DIST_SRC)/,$(DIST_SOURCES)) $(DIST_STAGE)/src/
	@cp $(addprefix $(DIST_GLES)/,$(DIST_GLES_FILES)) $(DIST_STAGE)/backend-gles/
	@cp $(addprefix $(DIST_STB)/,$(DIST_STB_FILES)) $(DIST_STAGE)/thirdparty/stb/
	@cp $(DIST_LIBVT)/*.c $(DIST_LIBVT)/*.h $(DIST_STAGE)/libvt/
	@cp $(DIST_SRC)/assets/font8x16.png $(DIST_STAGE)/assets/
	@cp $(DIST_SRC)/assets/fonts/DejaVuSans.ttf \
	    $(DIST_SRC)/assets/fonts/DejaVuSans-Bold.ttf \
	    $(DIST_SRC)/assets/fonts/DejaVuSans-Oblique.ttf \
	    $(DIST_SRC)/assets/fonts/DejaVuSans-BoldOblique.ttf \
	    $(DIST_SRC)/assets/fonts/DejaVuSansMono.ttf \
	    $(DIST_SRC)/assets/fonts/DejaVuSansMono-Bold.ttf \
	    $(DIST_SRC)/assets/fonts/DejaVuSansMono-Oblique.ttf \
	    $(DIST_SRC)/assets/fonts/DejaVuSansMono-BoldOblique.ttf \
	    $(DIST_SRC)/assets/fonts/DejaVuSans.LICENSE.txt $(DIST_STAGE)/assets/fonts/
	@cp $(DIST_SRC)/assets/pushpin/pushpin-out-14.png \
	    $(DIST_SRC)/assets/pushpin/pushpin-in-14.png $(DIST_STAGE)/assets/pushpin/
	@cp $(DIST_SRC)/README.md $(DIST_STAGE)/
	@cp $(DIST_SRC)/LICENSE.txt $(DIST_STAGE)/
	@cp $(DIST_SRC)/dist-module.mk $(DIST_STAGE)/module.mk
	@cp scripts/get-birdie-gui.sh $(DIST_STAGE)/
	@printf '%s\n' \
	    'birdie-gui $(GUI_VERSION)' \
	    '' \
	    'Portable retained-mode GUI toolkit (C). Source distribution.' \
	    'See README.md for usage.' \
	    '' \
	    '  include/        public API headers' \
	    '  src/            toolkit implementation + reference ludica/SDL3 backends' \
	    '  backend-gles/   raw X11/EGL/GLES backend + standalone widget gallery' \
	    '  libvt/          terminal escape-sequence engine (backs the VT widget)' \
	    '  thirdparty/stb/ vendored stb_truetype + stb_image (bundled)' \
	    '  assets/         chrome TTF (+ license), CP437 terminal atlas, pushpins' \
	    '  module.mk       backend-agnostic modular-make library build' \
	    '  LICENSE.txt     CC0 dedication + bundled third-party licenses' \
	    '  get-birdie-gui.sh  vendoring updater (fetch a release into your project)' \
	    '' \
	    'Backends (compile one into your own target; the library builds none):' \
	    '  src/bd_backend_ludica.c  needs ludica.' \
	    '  src/bd_backend_sdl3.c    needs SDL3 (pkg-config sdl3) + an ES3 loader.' \
	    '  backend-gles/            needs X11 + EGL + GLESv2, no ludica.' \
	    '  For another host, implement bd_backend (see include/bd_backend.h).' \
	    '' \
	    'Made by a machine. PUBLIC DOMAIN (CC0-1.0)' \
	    > $(DIST_STAGE)/MANIFEST.txt
	@cd $(OUTDIR) && zip -rq $(DIST_NAME).zip $(DIST_NAME)
	@echo "dist: wrote $(DIST_ZIP)"
