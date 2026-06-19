# Vendored miniz (single-file zlib-compatible library), built as a static lib.
# Used by bd_net for MCCP zlib-stream decompression. See UPSTREAM for the
# version and scripts/update-miniz.sh to refresh it.

LIBRARIES += miniz

miniz_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
miniz_SRCS  = miniz.c

miniz_CPPFLAGS          = -I$(miniz_DIR)
miniz_EXPORTED_CPPFLAGS = -I$(miniz_DIR)

# third-party; not held to birdie's -Wall -W
miniz_CFLAGS = -w
