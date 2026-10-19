# fldigi-psk-core -- plain make, C99, no dependencies besides libm and the pinned
# fldigi-dsp-core submodule dsp/.
#   make            static + shared library into $(BUILD) (after the dsp/ pin check)
#   make test       build and run the C tests (BPSK31/63/125 goldens, PSK variants vs fldigi)
#   make sanitize   the C tests under AddressSanitizer + UndefinedBehaviorSanitizer
#   make bench      CPU and memory benchmark (per submode)
#   make check-dsp  only the dsp/ pin check (dsp.lock, R-920.3)
#   make BUILD=/abs/dir shared   out-of-tree build (used by wave-owl)
BUILD ?= build
CC ?= cc
CFLAGS ?= -O3
DSP ?= dsp
# BPSK31/63/125 bit API (psk_rx.c, psk_tx.c): -ffp-contract=off, the arithmetic follows the
# frozen Python reference (wave-owl INV-750, INV-930).
CORE_FLAGS = -std=c99 -Wall -Wextra -Werror -pedantic -ffp-contract=off -fPIC -Iinclude
# PSK variants and dsp/: -ffp-contract=on, fuse a*b+c within one expression as fldigi's
# reference build (Apple clang -O2) does.
V_FLAGS = -std=c99 -Wall -Wextra -Werror -pedantic -ffp-contract=on -fPIC -Iinclude \
          -I$(DSP)/include
LDLIBS = -lm
TEST_FLAGS = -std=c99 -Wall -Wextra -Werror -pedantic -ffp-contract=off -Iinclude -I$(DSP)/include

UNAME := $(shell uname)
ifeq ($(UNAME),Darwin)
  SHLIB = libfldigi_psk.dylib
  SHFLAGS = -dynamiclib -install_name @rpath/libfldigi_psk.dylib
else
  SHLIB = libfldigi_psk.so
  SHFLAGS = -shared -Wl,-soname,libfldigi_psk.so
endif

DSP_SRC = fdsp_filters fdsp_fec fdsp_gfft
V_SRC = psk_v_common psk_v_rx psk_v_tx psk_v_varicode
V_HDRS = include/fldigi_psk.h src/psk_v_internal.h $(DSP)/include/fldigi_dsp.h
OBJ = $(BUILD)/psk_rx.o $(BUILD)/psk_tx.o $(patsubst %,$(BUILD)/%.o,$(V_SRC)) \
      $(patsubst %,$(BUILD)/dsp_%.o,$(DSP_SRC))

all: static shared
static: $(BUILD)/libfldigi_psk.a
shared: $(BUILD)/$(SHLIB)

$(BUILD):
	mkdir -p $(BUILD)

# The pin gate runs before anything is compiled and again whenever dsp/ or dsp.lock change.
$(BUILD)/dsp.ok: dsp.lock tools/check_dsp.sh $(wildcard $(DSP)/include/*.h $(DSP)/src/*.c) | $(BUILD)
	sh tools/check_dsp.sh dsp.lock $(DSP)
	touch $@

check-dsp:
	sh tools/check_dsp.sh dsp.lock $(DSP)

$(BUILD)/psk_rx.o $(BUILD)/psk_tx.o: $(BUILD)/%.o: src/%.c include/fldigi_psk.h | $(BUILD)
	$(CC) $(CORE_FLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/psk_v_%.o: src/psk_v_%.c $(V_HDRS) $(BUILD)/dsp.ok
	$(CC) $(V_FLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/dsp_%.o: $(DSP)/src/%.c $(V_HDRS) $(BUILD)/dsp.ok
	$(CC) $(V_FLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/libfldigi_psk.a: $(OBJ)
	ar rcs $@ $^

$(BUILD)/$(SHLIB): $(OBJ)
	$(CC) $(SHFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/test_psk: tests/test_psk.c $(BUILD)/libfldigi_psk.a
	$(CC) $(CORE_FLAGS) $(CFLAGS) $< $(BUILD)/libfldigi_psk.a -o $@ $(LDLIBS)

$(BUILD)/test_psk_tx: tests/test_psk_tx.c $(BUILD)/libfldigi_psk.a
	$(CC) $(CORE_FLAGS) $(CFLAGS) $< $(BUILD)/libfldigi_psk.a -o $@ $(LDLIBS)

$(BUILD)/test_psk_v: tests/test_psk_v.c tests/test_v_util.h $(BUILD)/libfldigi_psk.a
	$(CC) $(TEST_FLAGS) $(TEST_DEFS) $(CFLAGS) $< $(BUILD)/libfldigi_psk.a -o $@ $(LDLIBS)

$(BUILD)/test_psk_v_fldigi: tests/test_psk_v_fldigi.c tests/test_v_util.h $(BUILD)/libfldigi_psk.a
	$(CC) $(TEST_FLAGS) $(TEST_DEFS) $(CFLAGS) $< $(BUILD)/libfldigi_psk.a -o $@ $(LDLIBS)

test: $(BUILD)/test_psk $(BUILD)/test_psk_tx $(BUILD)/test_psk_v $(BUILD)/test_psk_v_fldigi
	$(BUILD)/test_psk tests/golden
	$(BUILD)/test_psk_tx tests/golden
	$(BUILD)/test_psk_v
	$(BUILD)/test_psk_v_fldigi tests/golden/variants

SAN_FLAGS = -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all

sanitize:
	$(MAKE) --no-print-directory BUILD=$(BUILD)/san CFLAGS="$(SAN_FLAGS)" \
		TEST_DEFS=-DPSKV_NOT_REFERENCE LDLIBS="$(LDLIBS) $(SAN_FLAGS)" test

$(BUILD)/bench_psk: tests/bench_psk.c $(BUILD)/libfldigi_psk.a
	$(CC) $(CORE_FLAGS) $(CFLAGS) $< $(BUILD)/libfldigi_psk.a -o $@ $(LDLIBS)

$(BUILD)/bench_psk_v: tests/bench_psk_v.c tests/test_v_util.h $(BUILD)/libfldigi_psk.a
	$(CC) $(TEST_FLAGS) $(CFLAGS) $< $(BUILD)/libfldigi_psk.a -o $@ $(LDLIBS)

bench: $(BUILD)/bench_psk $(BUILD)/bench_psk_v
	$(BUILD)/bench_psk tests/golden
	$(BUILD)/bench_psk_v

clean:
	rm -rf $(BUILD)

.PHONY: all static shared check-dsp test sanitize bench clean
