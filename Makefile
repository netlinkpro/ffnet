# SPDX-License-Identifier: LGPL-2.1-or-later
# ffnet — Makefile (Linux/x86_64, slice 1)
#
#   make             builds ./build/ffnet
#   make test        builds + runs unit tests (SHA-1, Base64, TCP)
#   make itest       runs the Python WS integration test (needs `websockets`)
#   make clean       removes ./build
#
# Tooling: NASM (-f elf64) for .asm, $(CC) for .c, ar for .a archives.

CC      ?= cc
AS      ?= nasm
AR      ?= ar
PYTHON  ?= python3

CFLAGS  ?= -O2 -g -Wall -Wextra -Wpedantic -std=c11 -fPIE -D_DEFAULT_SOURCE
LDFLAGS ?= -pie
ASFLAGS ?= -f elf64 -F dwarf -g -w+all

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
    $(BUILD)/libffnet/tcp_asm.o \
    $(BUILD)/libffnet/tcp_c.o
LIBFFNET = $(BUILD)/libffnet.a

# ---- libffproto -----------------------------------------------------------
LIBFFPROTO_OBJ = \
    $(BUILD)/libffproto/registry.o          \
    $(BUILD)/libffproto/ws/ws_handshake.o   \
    $(BUILD)/libffproto/ws/ws_frame.o       \
    $(BUILD)/libffproto/ws/ws_module.o
LIBFFPROTO = $(BUILD)/libffproto.a

# ---- CLI ------------------------------------------------------------------
CLI_OBJ = \
    $(BUILD)/cli/ffnet.o \
    $(BUILD)/cli/cmd_ws_echo.o

# Link order: late libs first (protocol → net → util).
LIBS = $(LIBFFPROTO) $(LIBFFNET) $(LIBFFUTIL)

# ---- Tests ----------------------------------------------------------------
TEST_BINS = \
    $(BUILD)/tests/test_sha1   \
    $(BUILD)/tests/test_base64 \
    $(BUILD)/tests/test_tcp

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
	$(AS) $(ASFLAGS) -o $@ $<

$(BUILD)/libffnet/tcp_asm.o: src/libffnet/tcp.asm
	@mkdir -p $(@D)
	$(AS) $(ASFLAGS) -o $@ $<

$(BUILD)/libffproto/ws/%.o: src/libffproto/ws/%.asm
	@mkdir -p $(@D)
	$(AS) $(ASFLAGS) -o $@ $<

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

# ===========================================================================
# Misc
# ===========================================================================
.PHONY: clean
clean:
	rm -rf $(BUILD)
