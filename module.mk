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
	src/birdie-gui \
	src/birdie-gui/bd_vt \
	src/birdie \
	test

# The widget gallery (src/guitest) needs X11/EGL/GLES and is Linux-only, so it
# is opt-in: its module.mk is loaded only when WIDGET_TEST is set, keeping the
# gallery out of the default `all`. The `widget-test` target below re-enters
# make with the flag set.
ifdef WIDGET_TEST
SUBDIRS += src/guitest
endif

# GL Loader configuration:
#
#   BD_GL_LOADER=builtin (default)  — birdie-gui provides a built-in GLES loader.
#                                      All GL entry points are resolved at runtime
#                                      via bd_gles_load_gl(getproc). No link-time
#                                      GL libs needed. Portable across Linux/Windows.
#   BD_GL_LOADER=external           — birdie-gui assumes GL entry points are
#                                      already available (GLEW, GLAD, Galogen,
#                                      direct linking, or a custom loader).
#                                      bd_gles_load_gl() becomes a no-op.
#
BD_GL_LOADER ?= builtin

# ----------------------------------------------------------------------
# Reference bd_backend bindings, declared here rather than in
# src/birdie-gui/module.mk on purpose. Because `all` builds compile_commands.json
# over every declared library's objects, declaring these next to the toolkit
# would force any consumer (including a dist-bundle build of just the core) to
# compile the ludica/GLES bindings and satisfy their host dependencies. Keeping
# them in the top-level (in-tree) build lets src/birdie-gui/module.mk stay
# host-neutral and ship verbatim as the bundle. The sources still live in
# src/birdie-gui/; only the library declarations live here.
#
#   birdie_gui_ludica     the reference ludica backend (bd_backend_ludica.c);
#                         the birdie executable links it.
#   birdie_gui_gles_core  the shared OpenGL ES 3 GPU core (bd_backend_gles_core.c)
#                         behind the raw X11/EGL/GLES gallery and the SDL3 example.
BD_GUI := src/birdie-gui
LIBRARIES += birdie_gui_ludica birdie_gui_gles_core

birdie_gui_ludica_DIR := $(BD_GUI)/
birdie_gui_ludica_SRCS = bd_backend_ludica.c
birdie_gui_ludica_LIBS = birdie_gui ludica

birdie_gui_gles_core_DIR := $(BD_GUI)/
birdie_gui_gles_core_SRCS = bd_backend_gles_core.c
birdie_gui_gles_core_LIBS = birdie_gui
birdie_gui_gles_core_CPPFLAGS = -I$(BD_GUI) -I$(BD_GUI)/thirdparty/stb

# GL loader configuration. Conditional bodies must not be tab-indented: a
# leading tab makes make treat the line as a recipe, which mangles the $(error)
# diagnostic below into a cryptic "recipe commences before first target".
ifeq ($(BD_GL_LOADER), builtin)
# Built-in loader: compile bd_gl.c and add the shim include dir to the path. The
# vendored Khronos headers come *after* $(BD_GUI) so the shim's #include_next
# resolves to them; this is what lets the loader compile on Windows/mingw, which
# ships no GLES3/gl3.h. On Linux they simply mirror the system headers.
birdie_gui_gles_core_SRCS += bd_gl.c
birdie_gui_gles_core_CPPFLAGS += -DBD_GL_LOADER_BUILTIN -I$(BD_GUI) -I$(BD_GUI)/thirdparty/khronos
else ifeq ($(BD_GL_LOADER), external)
# External loader: define the flag but don't compile bd_gl.c
birdie_gui_gles_core_CPPFLAGS += -DBD_GL_LOADER_EXTERNAL
else
$(error BD_GL_LOADER must be 'builtin' (default) or 'external', got: $(BD_GL_LOADER))
endif

# Shader-to-C generation using the vendored glsl2h.
$(BUILDDIR)/%.c : %.glsl $(LUDICA)/tools/glsl2h
	$(LUDICA)/tools/glsl2h $< > $@

# ----------------------------------------------------------------------
# Runtime assets staged next to the built executables.
#
# The toolkit names its built-in fonts only by their asset-root-relative
# sub-path (e.g. "fonts/DejaVuSans.ttf"); the backend's resolve_asset hook looks
# for them beside the executable. Copy the fonts into $(BINDIR) so a plain
# `make` run finds them there and the binary runs from any working directory.
# (The pushpins are 1-bit glyphs compiled into the toolkit, no asset file.)
# ----------------------------------------------------------------------
BD_ASSET_DIR := src/birdie-gui/assets
BD_FONT_OUT  := $(patsubst $(BD_ASSET_DIR)/fonts/%,$(BINDIR)/fonts/%,\
                  $(wildcard $(BD_ASSET_DIR)/fonts/*.ttf))

$(BINDIR)/fonts/% : $(BD_ASSET_DIR)/fonts/% | $(BINDIR)/fonts
	cp $< $@
$(BINDIR)/fonts :
	mkdir -p $@

all :: $(BD_FONT_OUT)

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
widget-test : $(BD_FONT_OUT)
	@$(MAKE) WIDGET_TEST=1 birdie-gui-gallery
	@echo "built widget gallery: $(BINDIR)/birdie-gui-gallery"
	@echo "run it from anywhere: $(BINDIR)/birdie-gui-gallery"

# Windows cross-compile smoke check. Builds the GUI libraries (the toolkit plus
# the GLES-core backend with the built-in loader) with the mingw-w64 toolchain.
# This is a compile+archive check of the actual Windows target the loader exists
# for: opengl32.dll exports only GL 1.1, so the ES 3.0 entry points must be
# resolved at runtime, which the built-in loader does. No GL runtime or import
# lib is needed (the loader defers gl* to runtime), so this catches Windows-only
# compile breaks that the Linux build cannot. Override the toolchain with
# WIN_CC=... (e.g. i686-w64-mingw32-gcc for 32-bit).
WIN_CC ?= x86_64-w64-mingw32-gcc
.PHONY : windows-check
windows-check :
	@triplet=`$(WIN_CC) -dumpmachine` && \
	    $(MAKE) CC=$(WIN_CC) _build/$$triplet/birdie_gui_gles_core.a && \
	    echo "windows cross-compile OK: _build/$$triplet/birdie_gui_gles_core.a"

# ----------------------------------------------------------------------
# Source distribution of the GUI toolkit (birdie-gui).
#
# The bundle is a self-contained copy of src/birdie-gui/: the same flat layout
# (sources + public headers together, its own thirdparty/stb, the bd_vt/
# terminal sublibrary, assets) and, crucially, the SAME module.mk file the
# in-tree build uses -- there is no separate dist-module.mk to drift. The
# reference backends ship as source to compile into your own target; the raw
# X11/EGL/GLES backend + gallery ship under backend-gles/. `make dist` stages
# this into a versioned ZIP under $(OUTDIR); override the version with
# `make dist GUI_VERSION=x.y.z`. Each backend still needs its host (ludica /
# SDL3 / X11+EGL).
# ----------------------------------------------------------------------
GUI_VERSION ?= 0.7.0
DIST_NAME   := birdie-gui-$(GUI_VERSION)
DIST_STAGE  := $(OUTDIR)/$(DIST_NAME)
DIST_ZIP    := $(OUTDIR)/$(DIST_NAME).zip
DIST_SRC    := src/birdie-gui
DIST_GLES   := src/guitest
DIST_STB    := src/birdie-gui/thirdparty/stb

# Public API headers, copied flat into the bundle root beside the sources (the
# same layout as src/birdie-gui). bd_widget_vt.h ships in bd_vt/ with its
# library, so a terminal-free consumer needn't see it.
DIST_HEADERS := widget.h widget_ext.h bd_backend.h bd_theme.h bd_draw.h \
                bd_asset.h bd_utf8.h bd_color.h bd_backend_gles_core.h bd_gl.h \
                bd_widget_value.h bd_widget_explorer.h \
                bd_widget_editor.h bd_widget_sketch.h bd_widget_table.h \
                bd_widget_inventory.h bd_widget_dock.h bd_widget_actionbar.h \
                bd_widget_tabview.h bd_widget_indicator.h \
                bd_widget_meter.h bd_widget_progress.h
# Toolkit implementation + reference ludica and SDL3 backends (source only: the
# bundle's module.mk declares just birdie_gui, so these compile only when a
# consumer adds them to their own target). No terminal here: the VT engine +
# widget ship as the separate birdie_gui_vt library in bd_vt/.
DIST_SOURCES := widget.c bd_draw.c bd_embed_font.h bd_asset.c bd_utf8.c bd_color.c \
                bd_widget_value.c \
                bd_widget_explorer.c bd_widget_editor.c bd_widget_sketch.c \
                bd_widget_table.c bd_widget_inventory.c bd_widget_dock.c \
                bd_widget_actionbar.c bd_widget_tabview.c \
                bd_widget_indicator.c bd_widget_meter.c bd_widget_progress.c \
                bd_embed_pushpin.h \
                pushpin_out_10.xbm pushpin_in_10.xbm \
                pushpin_out_14.xbm pushpin_in_14.xbm \
                bd_backend_gles_core.c bd_gl.c \
                bd_backend_ludica.c bd_backend_ludica.h \
                bd_backend_sdl3.c bd_backend_sdl3.h
# raw X11/EGL/GLES reference backend + widget gallery (Linux)
DIST_GLES_FILES := window.h x11_window.c bd_backend_gles.c bd_backend_gles.h \
                   widget_test.c
# vendored single-file libraries the toolkit includes (its own thirdparty/)
DIST_STB_FILES := stb_truetype.h stb_image.h
# vendored Khronos GLES3 headers + the loader's GLES3/gl3.h shim, so a consumer
# can build the GLES-core backend with the default built-in loader on a platform
# without system GLES headers (notably Windows). Mirrors the in-tree layout.
DIST_KHRONOS := src/birdie-gui/thirdparty/khronos
# the terminal library (VT engine + widget) — bundled so BD_TERMINAL compiles
DIST_VT     := src/birdie-gui/bd_vt

.PHONY : dist
dist :
	@rm -rf $(DIST_STAGE) $(DIST_ZIP)
	@mkdir -p $(DIST_STAGE)/backend-gles $(DIST_STAGE)/bd_vt \
	    $(DIST_STAGE)/thirdparty/stb $(DIST_STAGE)/GLES3 \
	    $(DIST_STAGE)/thirdparty/khronos/GLES3 \
	    $(DIST_STAGE)/thirdparty/khronos/KHR $(DIST_STAGE)/assets/fonts
	@cp $(addprefix $(DIST_SRC)/,$(DIST_HEADERS)) $(DIST_STAGE)/
	@cp $(addprefix $(DIST_SRC)/,$(DIST_SOURCES)) $(DIST_STAGE)/
	@cp $(DIST_SRC)/module.mk $(DIST_STAGE)/module.mk
	@cp $(addprefix $(DIST_GLES)/,$(DIST_GLES_FILES)) $(DIST_STAGE)/backend-gles/
	@cp $(addprefix $(DIST_STB)/,$(DIST_STB_FILES)) $(DIST_STAGE)/thirdparty/stb/
	@cp $(DIST_SRC)/GLES3/gl3.h $(DIST_STAGE)/GLES3/
	@cp $(DIST_KHRONOS)/GLES3/gl3.h $(DIST_KHRONOS)/GLES3/gl3platform.h \
	    $(DIST_STAGE)/thirdparty/khronos/GLES3/
	@cp $(DIST_KHRONOS)/KHR/khrplatform.h $(DIST_STAGE)/thirdparty/khronos/KHR/
	@cp $(DIST_KHRONOS)/UPSTREAM $(DIST_STAGE)/thirdparty/khronos/
	@cp $(DIST_VT)/*.c $(DIST_VT)/*.h $(DIST_VT)/module.mk $(DIST_STAGE)/bd_vt/
	@cp $(DIST_SRC)/assets/fonts/DejaVuSans.ttf \
	    $(DIST_SRC)/assets/fonts/DejaVuSans-Bold.ttf \
	    $(DIST_SRC)/assets/fonts/DejaVuSans-Oblique.ttf \
	    $(DIST_SRC)/assets/fonts/DejaVuSans-BoldOblique.ttf \
	    $(DIST_SRC)/assets/fonts/DejaVuSansMono.ttf \
	    $(DIST_SRC)/assets/fonts/DejaVuSansMono-Bold.ttf \
	    $(DIST_SRC)/assets/fonts/DejaVuSansMono-Oblique.ttf \
	    $(DIST_SRC)/assets/fonts/DejaVuSansMono-BoldOblique.ttf \
	    $(DIST_SRC)/assets/fonts/DejaVuSans.LICENSE.txt $(DIST_STAGE)/assets/fonts/
	@cp $(DIST_SRC)/README.md $(DIST_STAGE)/
	@cp $(DIST_SRC)/LICENSE.txt $(DIST_STAGE)/
	@cp scripts/get-birdie-gui.sh $(DIST_STAGE)/
	@printf '%s\n' \
	    'birdie-gui $(GUI_VERSION)' \
	    '' \
	    'Portable retained-mode GUI toolkit (C). Source distribution.' \
	    'A self-contained copy of the in-tree src/birdie-gui, built with the' \
	    'same module.mk. See README.md for usage.' \
	    '' \
	    '  module.mk       modular-make build of the birdie_gui library' \
	    '  *.c *.h         toolkit implementation + public headers (flat)' \
	    '  bd_backend_*.c  reference ludica / SDL3 / GLES-core backends (source)' \
	    '  bd_gl.c GLES3/  built-in GLES3 loader + its GLES3/gl3.h shim (runtime' \
	    '                  gl* resolution; needed on Windows, see README)' \
	    '  bd_vt/          terminal sublibrary: VT engine + BD_TERMINAL widget' \
	    '  backend-gles/   raw X11/EGL/GLES backend + standalone widget gallery' \
	    '  thirdparty/stb/ vendored stb_truetype + stb_image (bundled)' \
	    '  thirdparty/khronos/ vendored Khronos GLES3 headers for the loader' \
	    '  assets/         chrome TTF (+ license)' \
	    '  LICENSE.txt     CC0 dedication + bundled third-party licenses' \
	    '  get-birdie-gui.sh  vendoring updater (fetch a release into your project)' \
	    '' \
	    'Add this directory to SUBDIRS and birdie_gui to LIBS. For BD_TERMINAL,' \
	    'also add bd_vt/ to SUBDIRS and birdie_gui_vt to LIBS.' \
	    '' \
	    'Backends (compile one into your own target; the library builds none):' \
	    '  bd_backend_ludica.c  needs ludica.' \
	    '  bd_backend_sdl3.c    needs SDL3 (pkg-config sdl3) + an ES3 loader.' \
	    '  backend-gles/        needs X11 + EGL + GLESv2, no ludica.' \
	    '  For another host, implement bd_backend (see bd_backend.h).' \
	    '' \
	    'Made by a machine. PUBLIC DOMAIN (CC0-1.0)' \
	    > $(DIST_STAGE)/MANIFEST.txt
	@cd $(OUTDIR) && zip -rq $(DIST_NAME).zip $(DIST_NAME)
	@echo "dist: wrote $(DIST_ZIP)"
