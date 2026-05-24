# SPDX-License-Identifier: LGPL-2.1-or-later
# ffnet — Makefile (Linux/x86_64, slice 1)
#
#   make             builds ./build/ffnet
#   make test        builds + runs unit tests (SHA-1, Base64, TCP)
#   make itest       runs the Python WS integration test (needs `websockets`)
#   make clean       removes ./build
#
# Tooling: NASM (-f elf64) for .asm, $(CC) for .c, ar for .a archives.

CC       ?= cc
NASM     ?= nasm
AR       ?= ar
PYTHON   ?= python3

CFLAGS   ?= -O2 -g -Wall -Wextra -Wpedantic -std=c11 -fPIE -D_DEFAULT_SOURCE
LDFLAGS  ?= -pie -lpthread
# NB: do NOT use AS / ASFLAGS — make has builtin defaults (AS=as) that override `?=`.
NASMFLAGS ?= -f elf64 -F dwarf -g -w+all

INCLUDES = -I include
BUILD    = build

# ---- libffutil ------------------------------------------------------------
LIBFFUTIL_OBJ = \
    $(BUILD)/libffutil/sha1.o   \
    $(BUILD)/libffutil/base64.o \
    $(BUILD)/libffutil/buf.o    \
    $(BUILD)/libffutil/log.o
LIBFFUTIL = $(BUILD)/libffutil.a

# ---- libffnet -------------------------------------------------------------
# tcp.asm and tcp.c share a basename, so use distinct .o names.
LIBFFNET_OBJ = \
    $(BUILD)/libffnet/tcp_asm.o     \
    $(BUILD)/libffnet/tcp_c.o       \
    $(BUILD)/libffnet/eventloop.o   \
    $(BUILD)/libffnet/server.o
LIBFFNET = $(BUILD)/libffnet.a

# ---- libffproto -----------------------------------------------------------
LIBFFPROTO_OBJ = \
    $(BUILD)/libffproto/registry.o          \
    $(BUILD)/libffproto/ws/ws_handshake.o   \
    $(BUILD)/libffproto/ws/ws_frame.o       \
    $(BUILD)/libffproto/ws/ws_module.o      \
    $(BUILD)/libffproto/ws/utf8.o
LIBFFPROTO = $(BUILD)/libffproto.a

# ---- CLI ------------------------------------------------------------------
CLI_OBJ = \
    $(BUILD)/cli/ffnet.o       \
    $(BUILD)/cli/cmd_ws_echo.o \
    $(BUILD)/cli/cmd_tcp_echo.o

# Link order: late libs first (protocol → net → util).
LIBS = $(LIBFFPROTO) $(LIBFFNET) $(LIBFFUTIL)

# ---- Tests ----------------------------------------------------------------
TEST_BINS = \
    $(BUILD)/tests/test_sha1    \
    $(BUILD)/tests/test_base64  \
    $(BUILD)/tests/test_tcp     \
    $(BUILD)/tests/test_ffutil  \
    $(BUILD)/tests/test_evloop

# ===========================================================================
# Default target
# ===========================================================================
.PHONY: all
all: $(BUILD)/ffnet

$(BUILD)/ffnet: $(CLI_OBJ) $(LIBS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $(CLI_OBJ) $(LIBS) $(LDFLAGS)

# ===========================================================================
# Archive rules
# ===========================================================================
$(LIBFFUTIL): $(LIBFFUTIL_OBJ)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

$(LIBFFNET): $(LIBFFNET_OBJ)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

$(LIBFFPROTO): $(LIBFFPROTO_OBJ)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

# ===========================================================================
# Compile rules — C
# ===========================================================================
$(BUILD)/libffutil/%.o: src/libffutil/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(BUILD)/libffnet/tcp_c.o: src/libffnet/tcp.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Generic C compile for other libffnet .c files (eventloop.c, server.c).
# Make prefers the explicit tcp_c.o / tcp_asm.o rules above for tcp.*.
$(BUILD)/libffnet/%.o: src/libffnet/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(BUILD)/libffproto/%.o: src/libffproto/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(BUILD)/libffproto/ws/%.o: src/libffproto/ws/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(BUILD)/cli/%.o: cli/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# ===========================================================================
# Compile rules — Asm
# ===========================================================================
$(BUILD)/libffutil/%.o: src/libffutil/%.asm
	@mkdir -p $(@D)
	$(NASM) $(NASMFLAGS) -o $@ $<

$(BUILD)/libffnet/tcp_asm.o: src/libffnet/tcp.asm
	@mkdir -p $(@D)
	$(NASM) $(NASMFLAGS) -o $@ $<

$(BUILD)/libffproto/ws/%.o: src/libffproto/ws/%.asm
	@mkdir -p $(@D)
	$(NASM) $(NASMFLAGS) -o $@ $<

# ===========================================================================
# Tests
# ===========================================================================
.PHONY: test
test: $(TEST_BINS)
	@for t in $(TEST_BINS); do \
	    echo "==> $$t"; \
	    "$$t" || exit 1; \
	done
	@echo "all unit tests passed"

$(BUILD)/tests/test_%: tests/test_%.c $(LIBS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(LIBS) $(LDFLAGS)

.PHONY: itest
itest: $(BUILD)/ffnet
	$(PYTHON) tests/test_ws_echo.py $(BUILD)/ffnet

.PHONY: autobahn
autobahn: $(BUILD)/ffnet
	tests/autobahn/run.sh

# ===========================================================================
# Misc
# ===========================================================================
.PHONY: clean
clean:
	rm -rf $(BUILD)
