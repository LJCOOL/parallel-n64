# Host unit tests for the soft renderer: `make` builds and runs every test_*.c.
CC      ?= cc
CFLAGS  ?= -O1 -std=c99 -Wall -ffp-contract=off -I.. -I../../../../libretro-common/include
SRCS    := ../raster.c ../soft_xform.c ../spans.c ../spans_neon.c ../spans_fixed.c ../texcache.c ../combine.c ../queue.c
TESTS   := $(patsubst %.c,%,$(wildcard test_*.c))
BUILD   := build

all: $(addprefix $(BUILD)/,$(TESTS))
	@for t in $^; do ./$$t || exit 1; done
	@echo "all tests passed"

$(BUILD)/%: %.c $(SRCS) test.h gen_cmds.h $(wildcard ../*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(SRCS) -lm

# The thread test also covers stats lanes, so it links the stats module.
$(BUILD)/test_soft_mt: test_soft_mt.c $(SRCS) ../soft_mt.c ../soft_stats.c test.h gen_cmds.h $(wildcard ../*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DSOFT_STATS -o $@ $< $(SRCS) ../soft_mt.c ../soft_stats.c -lm -lpthread

clean:
	rm -rf $(BUILD)

.PHONY: all clean
