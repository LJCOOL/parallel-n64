# Host unit tests for the soft renderer: `make` builds and runs every test_*.c.
CC      ?= cc
CFLAGS  ?= -O1 -std=c99 -Wall -I.. -I../../../../libretro-common/include
SRCS    := ../raster.c ../soft_xform.c ../spans.c ../texcache.c ../combine.c
TESTS   := $(patsubst %.c,%,$(wildcard test_*.c))
BUILD   := build

all: $(addprefix $(BUILD)/,$(TESTS))
	@for t in $^; do ./$$t || exit 1; done
	@echo "all tests passed"

$(BUILD)/%: %.c $(SRCS) test.h $(wildcard ../*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(SRCS) -lm

clean:
	rm -rf $(BUILD)

.PHONY: all clean
