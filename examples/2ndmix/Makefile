# Copyright (c) 2026 David Reichelt. SPDX-License-Identifier: MIT
TARGET = 2ndmix-profiler.elf
OBJS = 2ndmix.o romdisk.o
VISPROF_DIR = ../..
VISPROF_ENABLED ?= 1
SAMPLE_DIR = $(KOS_BASE)/examples/dreamcast/2ndmix
KOS_ROMDISK_DIR = $(SAMPLE_DIR)/romdisk

KOS_CFLAGS := $(filter-out -DVISPROF_ENABLED=%,$(KOS_CFLAGS))
KOS_CFLAGS += -I$(SAMPLE_DIR) -I$(VISPROF_DIR)/include -DVISPROF_ENABLED=$(VISPROF_ENABLED)

all: $(TARGET)

include $(KOS_BASE)/Makefile.rules

2ndmix.c: $(SAMPLE_DIR)/2ndmix.c profiler.patch
	patch --batch --fuzz=0 -o $@.tmp $< profiler.patch
	mv $@.tmp $@

.build-mode: FORCE
	@echo "$(VISPROF_ENABLED)" | cmp -s - $@ || echo "$(VISPROF_ENABLED)" > $@

2ndmix.o: .build-mode $(VISPROF_DIR)/include/visprof/visprof.h $(SAMPLE_DIR)/sintab.h $(SAMPLE_DIR)/s3mplay.h $(SAMPLE_DIR)/gfx/font14_256.h
romdisk.img: $(wildcard $(KOS_ROMDISK_DIR)/*)

lib:
	$(MAKE) -C $(VISPROF_DIR) VISPROF_ENABLED=$(VISPROF_ENABLED)

$(TARGET): $(OBJS) lib
	kos-cc -o $@ $(OBJS) $(VISPROF_DIR)/libvisprof.a

baseline: 2ndmix-baseline.elf
2ndmix-baseline.elf: $(SAMPLE_DIR)/2ndmix.c romdisk.o $(SAMPLE_DIR)/sintab.h $(SAMPLE_DIR)/s3mplay.h $(SAMPLE_DIR)/gfx/font14_256.h
	kos-cc -o $@ $(SAMPLE_DIR)/2ndmix.c romdisk.o

clean:
	-rm -f $(OBJS) $(OBJS:.o=.d) $(TARGET) 2ndmix-baseline.elf 2ndmix.c 2ndmix.c.tmp romdisk.img romdisk_tmp.c romdisk_tmp.o .build-mode

FORCE:
.PHONY: all baseline clean lib FORCE
