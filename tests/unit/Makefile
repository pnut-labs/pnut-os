############################################################################
# pnut-os/tests/unit/Makefile
#
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Mateusz Pianka
#
############################################################################

# The unit tests: libpnut built for the computer, tested with cmocka
# (RFC 0022).  `make test` at the top runs them.
#
# CMOCKA_CFLAGS and CMOCKA_LIBS name cmocka where the compiler does not
# find it by itself.

ROOT   := $(abspath ../..)
LIB    := $(ROOT)/src/lib
BUILD  := $(ROOT)/build/unit

# The interfaces' code and nanopb: the top-level Makefile names them
# (make test)

GEN    ?= $(ROOT)/build/gen
NANOPB ?= $(firstword $(wildcard $(ROOT)/build/nanopb-*))

CMOCKA_CFLAGS ?=
CMOCKA_LIBS   ?= -lcmocka

CFLAGS  := -std=gnu11 -O1 -g -Wall -Werror -I$(LIB)/include -I$(LIB)
CFLAGS  += -I$(GEN) -I$(NANOPB) $(CMOCKA_CFLAGS)
LDLIBS  := $(CMOCKA_LIBS) -lpthread

# libpnut, the interfaces' generated code (make gen) and nanopb

SRCS    := $(wildcard $(LIB)/*.c) $(wildcard $(NANOPB)/pb_*.c)
SRCS    += $(shell find $(GEN) -name '*.c' 2> /dev/null)
HDRS    := $(wildcard $(LIB)/*.h $(LIB)/include/pnut/*.h)
HDRS    += $(shell find $(GEN) -name '*.h' 2> /dev/null)
TESTS   := $(patsubst %.c,$(BUILD)/%,$(wildcard test_*.c))

.PHONY: run

run: $(TESTS)
	@failed=0; for t in $(TESTS); do $$t || failed=1; done; exit $$failed

$(BUILD)/%: %.c $(SRCS) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(SRCS) $(LDLIBS)
