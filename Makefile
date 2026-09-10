# Copyright (c) 2026 David Reichelt. SPDX-License-Identifier: MIT
TARGET = libvisprof.a
OBJS = src/visprof.o src/visprof_draw.o

VISPROF_ENABLED ?= 1

KOS_CFLAGS := $(filter-out -DVISPROF_ENABLED=%,$(KOS_CFLAGS))
KOS_CFLAGS += -Iinclude -Isrc -DVISPROF_ENABLED=$(VISPROF_ENABLED)

all: $(TARGET)

.build-mode: FORCE
	@echo "$(VISPROF_ENABLED)" | cmp -s - $@ || echo "$(VISPROF_ENABLED)" > $@
FORCE:

$(OBJS): .build-mode include/visprof/visprof.h src/visprof_internal.h

$(TARGET): $(OBJS)
	rm -f $(TARGET)
	$(KOS_AR) rcs $(TARGET) $(OBJS)

check:
	@tools/check.sh

clean:
	$(MAKE) -C examples/basic clean
	$(MAKE) -C examples/2ndmix clean
	-rm -f $(OBJS) $(OBJS:.o=.d) $(TARGET) .build-mode

.PHONY: all check clean FORCE

include $(KOS_BASE)/Makefile.rules
