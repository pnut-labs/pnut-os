############################################################################
# pnut-os/Makefile
#
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Mateusz Pianka
#
############################################################################

# The one entry point for building pnut-os:
#
#   make <target>         configure NuttX for the target, and build it
#   make <target> run     build, then run it (the simulator, QEMU)
#   make <target> flash   build, then write it to the device
#   make style            check pnut-os's C code with NuttX's nxstyle
#   make clean            remove the build
#
# A target is a directory in configs/: its target.mk names the board's
# configuration, and its fragment.config holds pnut-os's options, merged
# over the board's by tools/configure-target.sh.

ROOT    := $(CURDIR)
NUTTX   := $(ROOT)/nuttx
APPS    := $(ROOT)/apps
BUILD   := $(ROOT)/build
PYTHON  ?= python3
JOBS    ?= $(shell nproc 2> /dev/null || echo 1)

# NuttX builds with -j$(JOBS), unless make was given its own -j; decided
# in the recipe, since make 4.3 sets MAKEFLAGS only after parsing

NUTTXJOBS = $(if $(filter -j% --jobserver%,$(MAKEFLAGS)),,-j$(JOBS))

TARGETS := $(patsubst configs/%/target.mk,%,$(wildcard configs/*/target.mk))
GOALS   := $(filter $(TARGETS),$(MAKECMDGOALS))
TARGET  := $(firstword $(GOALS))

.DEFAULT_GOAL := help
.PHONY: help $(TARGETS) run flash style clean submodules external force

ifneq ($(word 2,$(GOALS)),)
  $(error one target at a time: $(GOALS))
endif

ifneq ($(filter run flash,$(MAKECMDGOALS)),)
  ifeq ($(TARGET),)
    $(error name a target: make <target> run, make <target> flash; targets: $(TARGETS))
  endif
endif

help:
	@echo "make <target> [run|flash], make style, make clean"
	@echo "targets: $(TARGETS)"

# A target: configure when the target changes or its files do, then build.
# The stamp in build/ is removed first, so a configuration that fails or is
# interrupted is made again the next time; .config is kept even then, so
# that NuttX can distclean it.

ifneq ($(TARGET),)
include configs/$(TARGET)/target.mk

ifneq ($(filter run,$(MAKECMDGOALS)),)
  ifeq ($(RUN),)
    $(error target $(TARGET) cannot be run)
  endif
endif

ifneq ($(filter flash,$(MAKECMDGOALS)),)
  ifeq ($(FLASH),)
    $(error target $(TARGET) has no device to flash)
  endif
endif

ifneq ($(shell cat $(BUILD)/target 2> /dev/null),$(TARGET))
  RECONFIGURE := force
endif

$(TARGET): $(NUTTX)/.config
	$(MAKE) -C $(NUTTX) $(NUTTXJOBS)

.PRECIOUS: $(NUTTX)/.config

$(NUTTX)/.config: configs/$(TARGET)/target.mk configs/$(TARGET)/fragment.config $(RECONFIGURE) | submodules external
	rm -f $(BUILD)/target
	PYTHON=$(PYTHON) $(ROOT)/tools/configure-target.sh \
	  $(NUTTX) $(BOARD_CONFIG) $(ROOT)/configs/$(TARGET)/fragment.config
	mkdir -p $(BUILD)
	echo $(TARGET) > $(BUILD)/target

run: $(TARGET)
	$(RUN)

flash: $(TARGET)
	$(FLASH)
endif

force:

# Checks before configuring

submodules:
	@status=$$(git -C $(ROOT) submodule status) || exit 1; \
	echo "$$status" | awk ' \
	  /^-/ { print "error: submodule " $$2 " is not checked out: git submodule update --init" > "/dev/stderr"; bad = 1 } \
	  /^U/ { print "error: submodule " $$2 " has a merge conflict" > "/dev/stderr"; bad = 1 } \
	  /^\+/ { print "warning: submodule " $$2 " is not at the commit pnut-os records: git submodule update" > "/dev/stderr" } \
	  END { exit bad }'

external: | submodules
	@if [ -e $(APPS)/external ] && [ ! -L $(APPS)/external ]; then \
	  echo "error: $(APPS)/external is not a link" >&2; exit 1; \
	fi
	@ln -sfn ../src $(APPS)/external

# Style: NuttX's nxstyle on every C file of pnut-os, submodules apart

style: submodules
	$(MAKE) -C $(NUTTX)/tools -f Makefile.host nxstyle
	@status=0; \
	for f in $$(git -C $(ROOT) ls-files --cached --others --exclude-standard -- '*.c' '*.h'); do \
	  $(NUTTX)/tools/nxstyle $(ROOT)/$$f || status=1; \
	done; \
	exit $$status

clean:
	@if [ -f $(NUTTX)/.config ]; then $(MAKE) -C $(NUTTX) distclean; fi
	rm -rf $(BUILD)
