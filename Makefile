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
#   make test             run the unit tests, on the computer
#   make gen              generate the interfaces' code from proto/
#   make <target> test    build, then run the tests inside it (sim)
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

# nanopb, pinned (below), and the interfaces' code generated in build/gen

NANOPB_VERSION := 0.4.9.1
NANOPB_SHA256  := 882cd8473ad932b24787e676a808e4fb29c12e086d20bcbfbacc66c183094b5c
NANOPB_TARBALL := $(BUILD)/dl/nanopb-$(NANOPB_VERSION).tar.gz
NANOPB         := $(BUILD)/nanopb-$(NANOPB_VERSION)
NANOPB_LINK    := $(APPS)/netutils/nanopb/nanopb
GEN            := $(BUILD)/gen
PROTOS         := $(shell find proto tests/proto -name '*.proto' 2> /dev/null)

# pnut-os's files in /etc, laid over the board's (RFC 0010): NuttX's
# boards/Board.mk reads ETC_OVERLAY, from the environment here, so that
# every make of NuttX sees it, the build's and the flash's

export ETC_OVERLAY := $(ROOT)/etc

# NuttX builds with -j$(JOBS), unless make was given its own -j; decided
# in the recipe, since make 4.3 sets MAKEFLAGS only after parsing

NUTTXJOBS = $(if $(filter -j% --jobserver%,$(MAKEFLAGS)),,-j$(JOBS))

TARGETS := $(patsubst configs/%/target.mk,%,$(wildcard configs/*/target.mk))
GOALS   := $(filter $(TARGETS),$(MAKECMDGOALS))
TARGET  := $(firstword $(GOALS))

.DEFAULT_GOAL := help
.PHONY: help $(TARGETS) run flash test style clean submodules external force
.PHONY: nanopb gen

ifneq ($(word 2,$(GOALS)),)
  $(error one target at a time: $(GOALS))
endif

ifneq ($(filter run flash,$(MAKECMDGOALS)),)
  ifeq ($(TARGET),)
    $(error name a target: make <target> run, make <target> flash; targets: $(TARGETS))
  endif
endif

ifneq ($(filter test,$(MAKECMDGOALS)),)
  ifneq ($(TARGET),)
    TARGETTEST := y
  endif
endif

help:
	@echo "make <target> [run|flash], make test, make style, make clean"
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

ifeq ($(TARGETTEST),y)
  ifeq ($(TEST),)
    $(error target $(TARGET) has no tests)
  endif
endif

ifneq ($(shell cat $(BUILD)/target 2> /dev/null),$(TARGET))
  RECONFIGURE := force
endif

# The board's own configuration, <board>:<config>, which a move of the
# nuttx submodule may change: configured afresh then too

BOARD_DEFCONFIG := $(wildcard $(NUTTX)/boards/*/*/$(firstword \
                     $(subst :, ,$(BOARD_CONFIG)))/configs/$(lastword \
                     $(subst :, ,$(BOARD_CONFIG)))/defconfig)

# Configuring runs nuttx-apps' distclean, which removes the nanopb link:
# it is made again here, after configuring, before building

$(TARGET): $(NUTTX)/.config $(GEN)/.stamp | external
	$(nanopb_link)
	$(MAKE) -C $(NUTTX) $(NUTTXJOBS)

.PRECIOUS: $(NUTTX)/.config

$(NUTTX)/.config: configs/$(TARGET)/target.mk configs/$(TARGET)/fragment.config $(BOARD_DEFCONFIG) $(RECONFIGURE) | submodules external nanopb
	rm -f $(BUILD)/target
	PYTHON=$(PYTHON) $(ROOT)/tools/configure-target.sh \
	  $(NUTTX) $(BOARD_CONFIG) $(ROOT)/configs/$(TARGET)/fragment.config
	mkdir -p $(BUILD)
	echo $(TARGET) > $(BUILD)/target

run: $(TARGET)
	$(RUN)

flash: $(TARGET)
	$(FLASH)

ifeq ($(TARGETTEST),y)
test: $(TARGET)
	$(TEST)
endif
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

# nanopb (RFC 0023), pinned and checked, in a directory named for its
# version, so that a new version is fetched.  NuttX's build compiles it
# through nuttx-apps' package, which finds it at the link instead of
# downloading its own; the unit tests compile it from here; its generator
# runs on the computer.

define nanopb_link
	@if [ -e $(NANOPB_LINK) ] && [ ! -L $(NANOPB_LINK) ]; then \
	  echo "error: $(NANOPB_LINK) is nuttx-apps' own download: remove it" >&2; \
	  exit 1; \
	fi
	@ln -sfn $(NANOPB) $(NANOPB_LINK)
endef

nanopb: $(NANOPB)/pb.h | submodules
	$(nanopb_link)

$(NANOPB)/pb.h:
	rm -rf $(NANOPB) $(NANOPB).tmp
	mkdir -p $(BUILD)/dl $(NANOPB).tmp
	curl -fsSL -o $(NANOPB_TARBALL) \
	  https://jpa.kapsi.fi/nanopb/download/nanopb-$(NANOPB_VERSION).tar.gz
	echo "$(NANOPB_SHA256)  $(NANOPB_TARBALL)" | sha256sum -c -
	tar xzf $(NANOPB_TARBALL) -C $(NANOPB).tmp --strip-components=1
	mv $(NANOPB).tmp $(NANOPB)

# The interfaces' code (RFC 0023), made afresh in build/gen whenever a
# .proto file or the generator changes, or a .proto file comes or goes
# (the list of them changes); never committed

gen: $(GEN)/.stamp

$(BUILD)/protos: force
	@mkdir -p $(BUILD)
	@printf '%s\n' $(sort $(PROTOS)) > $@.new
	@if cmp -s $@.new $@; then rm $@.new; else mv $@.new $@; fi

$(GEN)/.stamp: $(PROTOS) $(BUILD)/protos tools/protoc-gen-pnut tools/generate.sh $(NANOPB)/pb.h
	$(ROOT)/tools/generate.sh $(ROOT) $(GEN) $(PYTHON) $(NANOPB)
	touch $@

# The unit tests: libpnut built for the computer, with cmocka

ifneq ($(TARGETTEST),y)
test: $(GEN)/.stamp
	$(MAKE) -C tests/unit GEN=$(GEN) NANOPB=$(NANOPB)
endif

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
