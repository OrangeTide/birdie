bd_vt_DIR  := $(dir $(lastword $(MAKEFILE_LIST)))
bd_vt_SRCS  = vt_parse.c vt_buf.c vt_cell.c vt_state.c vt_ops.c \
              utf8.c rune_width.c width_tables.c xmalloc.c
bd_vt_EXPORTED_CPPFLAGS = -I$(bd_vt_DIR)

LIBRARIES += bd_vt
