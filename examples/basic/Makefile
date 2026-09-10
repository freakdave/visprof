# Copyright (c) 2026 David Reichelt. SPDX-License-Identifier: MIT
TARGET = visprof_demo.elf
OBJS = main.o

VISPROF_DIR = ../..
VISPROF_ENABLED ?= 1

KOS_CFLAGS := $(filter-out -DVISPROF_ENABLED=%,$(KOS_CFLAGS))
KOS_CFLAGS += -I$(VISPROF_DIR)/include -DVISPROF_ENABLED=$(VISPROF_ENABLED)

all: $(TARGET)

include $(KOS_BASE)/Makefile.rules

.build-mode: FORCE
	@echo "$(VISPROF_ENABLED)" | cmp -s - $@ || echo "$(VISPROF_ENABLED)" > $@
FORCE:

$(OBJS): .build-mode $(VISPROF_DIR)/include/visprof/visprof.h

lib:
	$(MAKE) -C $(VISPROF_DIR) VISPROF_ENABLED=$(VISPROF_ENABLED)

$(TARGET): $(OBJS) lib
	kos-cc -o $(TARGET) $(OBJS) $(VISPROF_DIR)/libvisprof.a -lm

clean: rm-elf
	-rm -f $(OBJS) $(OBJS:.o=.d) .build-mode

rm-elf:
	-rm -f $(TARGET)

.PHONY: all clean rm-elf lib FORCE
