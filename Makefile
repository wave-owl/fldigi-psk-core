# fldigi-psk-core -- plain make, C99, no dependencies besides libm.
#   make            static + shared library into $(BUILD)
#   make test       build and run the C tests
#   make bench      CPU benchmark (60 min of audio per submode)
#   make BUILD=/abs/dir shared   out-of-tree build (used by wave-owl)
BUILD ?= build
CC ?= cc
# -ffp-contract=off: no fused multiply-add, the arithmetic follows the reference order.
CFLAGS ?= -O3
CORE_FLAGS = -std=c99 -Wall -Wextra -Werror -pedantic -ffp-contract=off -fPIC -Iinclude
LDLIBS = -lm

UNAME := $(shell uname)
ifeq ($(UNAME),Darwin)
  SHLIB = libfldigi_psk.dylib
  SHFLAGS = -dynamiclib -install_name @rpath/libfldigi_psk.dylib
else
  SHLIB = libfldigi_psk.so
  SHFLAGS = -shared -Wl,-soname,libfldigi_psk.so
endif

OBJ = $(BUILD)/psk_rx.o

all: static shared
static: $(BUILD)/libfldigi_psk.a
shared: $(BUILD)/$(SHLIB)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/psk_rx.o: src/psk_rx.c include/fldigi_psk.h | $(BUILD)
	$(CC) $(CORE_FLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/libfldigi_psk.a: $(OBJ)
	ar rcs $@ $^

$(BUILD)/$(SHLIB): $(OBJ)
	$(CC) $(SHFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/test_psk: tests/test_psk.c $(BUILD)/libfldigi_psk.a
	$(CC) $(CORE_FLAGS) $(CFLAGS) $< $(BUILD)/libfldigi_psk.a -o $@ $(LDLIBS)

test: $(BUILD)/test_psk
	$(BUILD)/test_psk tests/golden

$(BUILD)/bench_psk: tests/bench_psk.c $(BUILD)/libfldigi_psk.a
	$(CC) $(CORE_FLAGS) $(CFLAGS) $< $(BUILD)/libfldigi_psk.a -o $@ $(LDLIBS)

bench: $(BUILD)/bench_psk
	$(BUILD)/bench_psk tests/golden

clean:
	rm -rf $(BUILD)

.PHONY: all static shared test bench clean
