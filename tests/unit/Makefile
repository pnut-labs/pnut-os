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
RADIOS := $(ROOT)/src/radios
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
CFLAGS  += -I$(RADIOS) -I$(SENSORS)
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

# The radios program's Radio; its LoRa radio is NuttX's SX126x driver:
# the tests give a fake

SRCS    += $(RADIOS)/packetradio.c $(RADIOS)/rules.c

# The sensors program's Location, whose receiver speaks UBX through
# NuttX's GNSS driver, and Motion, whose hub is NuttX's BHI260AP driver:
# the tests give fakes

SRCS    += $(SENSORS)/location.c $(SENSORS)/ubx.c $(SENSORS)/motion.c
HDRS    := $(wildcard $(LIB)/*.h $(LIB)/include/pnut/*.h $(SYSTEM)/*.h)
HDRS    += $(wildcard $(TELEPHONY)/*.h $(DATA)/*.h $(NETWORK)/*.h)
HDRS    += $(wildcard $(MEDIA)/*.h $(RADIOS)/*.h $(SENSORS)/*.h)
HDRS    += $(shell find $(GEN) -name '*.h' 2> /dev/null)
TESTS   := $(patsubst %.c,$(BUILD)/%,$(wildcard test_*.c))

# The system UI's tests (test_ui_*): its files but those of the device
# alone, the fonts and icons made for it (build/ui), and LVGL, built once
# into a library from lv_conf.h here, which follows the LVGL options of
# configs/tdeck-max/fragment.config.  The top-level Makefile names LVGL
# (make test).

LVGL    ?= $(firstword $(wildcard $(ROOT)/build/lvgl-*))
UI      := $(ROOT)/src/ui
UIGEN   := $(ROOT)/build/ui
UI_SRCS := $(filter-out %/main.c %/fb.c %/input_dev.c,$(wildcard $(UI)/*.c))
UI_SRCS += $(wildcard $(UIGEN)/*.c)
UI_HDRS := $(wildcard $(UI)/*.h $(UIGEN)/*.h) lv_conf.h

LVGL_CFLAGS := -std=gnu11 -O1 -g -DLV_CONF_INCLUDE_SIMPLE -I$(CURDIR)
LVGL_CFLAGS += -I$(LVGL) -I$(BUILD)/inc
LVGL_NAME   := $(notdir $(LVGL))
LVGL_SRCS   := $(shell find $(LVGL)/src -name '*.c' 2> /dev/null)
LVGL_OBJS   := $(patsubst $(LVGL)/%.c,$(BUILD)/$(LVGL_NAME)/%.o,$(LVGL_SRCS))
LVGL_LIB    := $(BUILD)/lib$(LVGL_NAME).a

.PHONY: run screens

# The screens' tests write the screens they draw to $(BUILD)/screens,
# made afresh each run

SCREEN_TESTS := $(BUILD)/test_ui_screens $(BUILD)/test_ui_wifi

run: $(TESTS)
	@rm -rf $(BUILD)/screens
	@failed=0; for t in $(TESTS); do $$t || failed=1; done; exit $$failed

screens: $(SCREEN_TESTS)
	@rm -rf $(BUILD)/screens
	@failed=0; for t in $(SCREEN_TESTS); do $$t || failed=1; done; \
	exit $$failed

$(BUILD)/test_ui_%: test_ui_%.c $(SRCS) $(HDRS) $(UI_SRCS) $(UI_HDRS) \
                    $(LVGL_LIB) | $(BUILD)/inc/lvgl
	$(CC) $(CFLAGS) $(LVGL_CFLAGS) -I$(UI) -I$(UIGEN) \
	  -DSCREENS_DIR=\"$(BUILD)/screens\" -o $@ $< $(SRCS) $(UI_SRCS) \
	  $(LVGL_LIB) $(LDLIBS)

$(BUILD)/%: %.c $(SRCS) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(SRCS) $(LDLIBS)

# Named for LVGL's version, so that another version is built afresh.  The
# link makes it <lvgl/lvgl.h>, as nuttx-apps gives it on the device.

$(LVGL_LIB): $(LVGL_OBJS)
	@rm -f $@
	@$(AR) rcs $@ $^
	@mkdir -p $(BUILD)/inc
	@ln -sfn $(LVGL) $(BUILD)/inc/lvgl

$(BUILD)/inc/lvgl:
	@mkdir -p $(BUILD)/inc
	@ln -sfn $(LVGL) $@

$(BUILD)/$(LVGL_NAME)/%.o: $(LVGL)/%.c lv_conf.h
	@mkdir -p $(dir $@)
	@$(CC) $(LVGL_CFLAGS) -c -o $@ $<
