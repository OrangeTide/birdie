#!/bin/sh
# scripts/update-khronos.sh — refresh the vendored Khronos GLES3 headers.
#
# Usage:
#   scripts/update-khronos.sh                  # tip of both registries (main)
#   scripts/update-khronos.sh <gl_ref>         # pin OpenGL registry; EGL = same
#   scripts/update-khronos.sh <gl_ref> <egl_ref>  # pin each registry separately
#
# gl3.h + gl3platform.h come from the OpenGL registry, khrplatform.h from the
# EGL registry — two separate repos with independent histories. Neither
# publishes tags/releases, so an immutable pin is a commit SHA, and because a
# SHA only exists in one repo you must give both refs to pin (a GL SHA does not
# resolve in the EGL registry). Branch names like `main` work for both.
#
# Environment variables:
#   GL_REGISTRY_REPO    OpenGL registry (default: KhronosGroup/OpenGL-Registry)
#   EGL_REGISTRY_REPO   EGL registry    (default: KhronosGroup/EGL-Registry)
#
# The built-in GL loader's GLES3/gl3.h shim #include_next's these headers for
# the GL types, enums, and PFNGL*PROC typedefs. Windows/mingw ships
# KHR/khrplatform.h but no GLES3/gl3.h, so we vendor the Khronos ES 3.0 core
# headers under src/birdie-gui/thirdparty/khronos/. Only the core gl3.h is
# vendored (the backend uses no extensions).

set -eu

GL_REGISTRY_REPO="${GL_REGISTRY_REPO:-KhronosGroup/OpenGL-Registry}"
EGL_REGISTRY_REPO="${EGL_REGISTRY_REPO:-KhronosGroup/EGL-Registry}"
HERE=$(cd -- "$(dirname "$0")/.." && pwd)
DEST=$HERE/src/birdie-gui/thirdparty/khronos

GL_REF="${1:-main}"
EGL_REF="${2:-$GL_REF}"

if command -v curl >/dev/null 2>&1; then
	fetch() { curl -fsSL "$1"; }
elif command -v wget >/dev/null 2>&1; then
	fetch() { wget -qO- "$1"; }
else
	echo "update-khronos: need curl or wget" >&2
	exit 1
fi

GL_RAW="https://raw.githubusercontent.com/${GL_REGISTRY_REPO}/${GL_REF}/api"
EGL_RAW="https://raw.githubusercontent.com/${EGL_REGISTRY_REPO}/${EGL_REF}/api"

tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT

# fetch_to <url> <dest> <sentinel>: download and sanity-check the result so a
# 404 page or a redirect never gets vendored in place of a real header.
fetch_to() {
	if ! fetch "$1" > "$2"; then
		echo "update-khronos: failed to download $1" >&2
		exit 1
	fi
	if ! grep -q "$3" "$2"; then
		echo "update-khronos: $1 does not look like the expected header" >&2
		exit 1
	fi
}

fetch_to "$GL_RAW/GLES3/gl3.h"         "$tmp/gl3.h"         "GL_ES_VERSION_3_0"
fetch_to "$GL_RAW/GLES3/gl3platform.h" "$tmp/gl3platform.h" "gl3platform"
fetch_to "$EGL_RAW/KHR/khrplatform.h"  "$tmp/khrplatform.h" "__khrplatform_h_"

mkdir -p "$DEST/GLES3" "$DEST/KHR"
cp "$tmp/gl3.h" "$tmp/gl3platform.h" "$DEST/GLES3/"
cp "$tmp/khrplatform.h" "$DEST/KHR/"

synced_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)

cat > "$DEST/UPSTREAM" <<EOF
Khronos OpenGL ES 3.0 API headers
=================================

Vendored so birdie-gui's built-in GL loader (BD_GL_LOADER=builtin) can compile
on platforms that do not ship GLES headers, notably Windows/mingw (which
provides KHR/khrplatform.h but no GLES3/gl3.h). On Linux these mirror the
system headers; vendoring them makes the toolkit self-contained and keeps the
same header across every target.

Files:
  GLES3/gl3.h          OpenGL ES 3.0 core API      (SPDX: MIT)
  GLES3/gl3platform.h  platform typedefs           (SPDX: Apache-2.0)
  KHR/khrplatform.h    Khronos platform typedefs   (Khronos MIT-style notice)

Source: the Khronos registries (fetch with scripts/update-khronos.sh)
  gl3.h, gl3platform.h  https://github.com/${GL_REGISTRY_REPO} (api/GLES3/)
                        ref ${GL_REF}
  khrplatform.h         https://github.com/${EGL_REGISTRY_REPO} (api/KHR/)
                        ref ${EGL_REF}
Copyright 2008-2020 The Khronos Group Inc. Unmodified; original license
notices are preserved in each file.

synced_at: ${synced_at}

Only the core gl3.h is vendored (the backend uses no extensions, so gl2ext.h
is intentionally omitted).
EOF

echo "update-khronos: vendored GLES3 headers (gl:$GL_REF egl:$EGL_REF) into $DEST"
