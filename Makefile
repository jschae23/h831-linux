# SPDX-License-Identifier: GPL-2.0
#
# Top-level orchestrator - builds each module with a separate M=
# invocation (the in-tree kbuild machinery only supports one external
# module per directory).
#
# Users need linux-headers for their running kernel:
#   sudo apt install linux-headers-$(uname -r)
#
# Modules produced:
#   cx231xx/cx231xx-h831.ko
#   mn88473/mn88473.ko   (H831 demod is an MN88473)
#   mxl601/mxl601.ko     (H831 tuner)

KVER   ?= $(shell uname -r)
KDIR   ?= /lib/modules/$(KVER)/build
PWD    ?= $(shell pwd)

# kbuild must run as the invoking user, not root. Only the copy/depmod
# steps of `load` need sudo.
ifeq ($(shell id -u),0)
BUILD_GOALS := $(strip $(if $(MAKECMDGOALS),$(MAKECMDGOALS),.DEFAULT))
NON_ROOT_OK := load unload clean reload
ifneq ($(filter-out $(NON_ROOT_OK),$(BUILD_GOALS)),)
$(error Do not build as root. Run 'make modules' as your normal user, then 'sudo make load')
endif
endif

modules: FORCE
	$(MAKE) -C $(KDIR) H831_ROOT='$(PWD)' M='$(PWD)/mn88473' modules
	$(MAKE) -C $(KDIR) H831_ROOT='$(PWD)' M='$(PWD)/mxl601' modules
	$(MAKE) -C $(KDIR) H831_ROOT='$(PWD)' M='$(PWD)/cx231xx' modules

clean:
	-$(MAKE) -C $(KDIR) M='$(PWD)/mn88473' clean
	-$(MAKE) -C $(KDIR) M='$(PWD)/mxl601' clean
	-$(MAKE) -C $(KDIR) M='$(PWD)/cx231xx' clean

# NOTE: `load` does not build (a sudo invocation would run kbuild as
# root). Run `make modules` as yourself first, then `sudo make load`
# only copies and runs depmod.
load:
	@test -f mn88473/mn88473.ko -a -f mxl601/mxl601.ko -a -f cx231xx/cx231xx-h831.ko || \
		{ echo "ERROR: modules not built yet - run 'make modules' first"; exit 1; }
	sudo mkdir -p "/lib/modules/$(KVER)/updates/"
	sudo cp mn88473/mn88473.ko mxl601/mxl601.ko \
	        cx231xx/cx231xx-h831.ko "/lib/modules/$(KVER)/updates/"
	sudo depmod -a $(KVER)

# Full rebuild round-trip for bring-up: swap the deployed copies, drop
# the resident instances, then load the fresh set. Run `make modules`
# first. The stick must be unplugged.
reload:
	@if ls /sys/bus/usb/drivers/cx231xx-h831/ 2>/dev/null | grep -q ':' ; then \
		echo "ERROR: tuner is still bound to the driver - unplug the stick first"; exit 1; \
	fi
	@test -f mn88473/mn88473.ko -a -f cx231xx/cx231xx-h831.ko || \
		{ echo "ERROR: modules not built - run 'make modules' first"; exit 1; }
	sudo cp -f mn88473/mn88473.ko mxl601/mxl601.ko \
	        cx231xx/cx231xx-h831.ko "/lib/modules/$(KVER)/updates/"
	sudo depmod -a $(KVER)
	-sudo rmmod cx231xx_dvb
	-sudo rmmod cx231xx_alsa
	-sudo rmmod cx231xx_h831
	-sudo rmmod mxl601
	-sudo rmmod mn88473
	sudo modprobe mn88473
	sudo modprobe mxl601
	sudo insmod cx231xx/cx231xx-h831.ko
	@echo "fresh modules loaded - replug the stick"

unload:
	sudo depmod -a $(KVER)
	-sudo modprobe -r cx231xx-h831
	-sudo modprobe -r mxl601
	-sudo modprobe -r mn88473

.PHONY: modules clean load unload reload FORCE

.DELETE_ON_ERROR:
