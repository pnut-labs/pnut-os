############################################################################
# pnut-os/tests/unit/Makefile
#
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Mateusz Pianka
#
############################################################################

# The unit tests: libpnut, and the system, telephony and data programs'
# modules, built for the computer, tested with cmocka (RFC 0022).
# `make test` at the top runs them.
#
# CMOCKA_CFLAGS and CMOCKA_LIBS name cmocka where the compiler does not
# find it by itself.

ROOT   := $(abspath ../..)
LIB    := $(ROOT)/src/lib
SYSTEM := $(ROOT)/src/system
TELEPHONY := $(ROOT)/src/telephony
DATA   := $(ROOT)/src/data
NETWORK := $(ROOT)/src/network
MEDIA  := $(ROOT)/src/media
SENSORS := $(ROOT)/src/sensors
BUILD  := $(ROOT)/build/unit

# The interfaces' code and nanopb: the top-level Makefile names them
# (make test)

GEN    ?= $(ROOT)/build/gen
NANOPB ?= $(firstword $(wildcard $(ROOT)/build/nanopb-*))

CMOCKA_CFLAGS ?=
CMOCKA_LIBS   ?= -lcmocka

CFLAGS  := -std=gnu11 -O1 -g -Wall -Werror -I$(LIB)/include -I$(LIB)
CFLAGS  += -I$(SYSTEM) -I$(TELEPHONY) -I$(DATA) -I$(NETWORK) -I$(MEDIA)
CFLAGS  += -I$(SENSORS)
CFLAGS  += -I$(GEN) -I$(NANOPB) $(CMOCKA_CFLAGS)

# Time's NTP, which the host's network can serve the tests

CFLAGS  += -DCONFIG_PNUT_TIME_NTP=1
# Settings' secrets: libsodium (libsodium-dev), as on the targets

LDLIBS  := $(CMOCKA_LIBS) -lsodium -lpthread

# libpnut, the programs' modules, the interfaces' generated code
# (make gen) and nanopb

SRCS    := $(wildcard $(LIB)/*.c) $(wildcard $(NANOPB)/pb_*.c)
SRCS    += $(shell find $(GEN) -name '*.c' 2> /dev/null)
SRCS    += $(filter-out %/main.c,$(wildcard $(SYSTEM)/*.c))
SRCS    += $(filter-out %/main.c,$(wildcard $(TELEPHONY)/*.c))
SRCS    += $(filter-out %/main.c,$(wildcard $(DATA)/*.c))

# The network program's Wi-Fi; its radio is NuttX's driver: the tests
# give a fake

SRCS    += $(NETWORK)/network.c $(NETWORK)/wifi.c

# The media program's Haptics; its motor is NuttX's force-feedback driver:
# the tests give a fake

SRCS    += $(MEDIA)/haptics.c

# The sensors program's Location; its receiver speaks UBX through NuttX's
# GNSS driver: the tests give a fake

SRCS    += $(SENSORS)/location.c $(SENSORS)/ubx.c
HDRS    := $(wildcard $(LIB)/*.h $(LIB)/include/pnut/*.h $(SYSTEM)/*.h)
HDRS    += $(wildcard $(TELEPHONY)/*.h $(DATA)/*.h $(NETWORK)/*.h)
HDRS    += $(wildcard $(MEDIA)/*.h $(SENSORS)/*.h)
HDRS    += $(shell find $(GEN) -name '*.h' 2> /dev/null)
TESTS   := $(patsubst %.c,$(BUILD)/%,$(wildcard test_*.c))

.PHONY: run

run: $(TESTS)
	@failed=0; for t in $(TESTS); do $$t || failed=1; done; exit $$failed

$(BUILD)/%: %.c $(SRCS) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(SRCS) $(LDLIBS)
