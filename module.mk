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
GUI_VERSION ?= 0.1.0
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
	@printf '%s\n' \
	    'birdie-gui $(GUI_VERSION)' \
	    '' \
	    'Portable retained-mode GUI toolkit (C). Source distribution.' \
	    '' \
	    'Layout:' \
	    '  include/  public API headers' \
	    '  src/      toolkit implementation + reference ludica backend' \
	    '  assets/   runtime assets loaded by the widgets' \
	    '' \
	    'API headers:' \
	    '  widget.h        app API (build UIs from widgets)' \
	    '  widget_ext.h    extension API (define new widget types)' \
	    '  bd_backend.h    renderer/window backend interface' \
	    '  bd_theme.h      configurable chrome theme' \
	    '  bd_widget_vt.h  terminal widget (extension)' \
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
