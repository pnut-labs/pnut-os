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

ROOT  := $(abspath ../..)
LIB   := $(ROOT)/src/lib
BUILD := $(ROOT)/build/unit

CMOCKA_CFLAGS ?=
CMOCKA_LIBS   ?= -lcmocka

CFLAGS  := -std=gnu11 -O1 -g -Wall -Werror -I$(LIB)/include -I$(LIB)
CFLAGS  += $(CMOCKA_CFLAGS)
LDLIBS  := $(CMOCKA_LIBS) -lpthread

SRCS    := $(wildcard $(LIB)/*.c)
HDRS    := $(wildcard $(LIB)/*.h $(LIB)/include/pnut/*.h)
TESTS   := $(patsubst %.c,$(BUILD)/%,$(wildcard test_*.c))

.PHONY: run

run: $(TESTS)
	@failed=0; for t in $(TESTS); do $$t || failed=1; done; exit $$failed

$(BUILD)/%: %.c $(SRCS) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(SRCS) $(LDLIBS)
