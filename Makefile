CC = clang
CFLAGS = -O3 -march=native -mtune=native -flto -ffast-math -funroll-loops -finline-functions
CFLAGS += -fomit-frame-pointer -fno-stack-protector
CFLAGS += -fdata-sections -ffunction-sections
CFLAGS += -falign-functions=64 -falign-loops=64
CFLAGS += -fno-plt -fprefetch-loop-arrays
CFLAGS += -fvectorize -fslp-vectorize
CFLAGS += -mllvm -polly
CFLAGS += -w -I/usr/local/include -Iobject
LDFLAGS = -flto -L/usr/local/lib
LDFLAGS += -Wl,--gc-sections -Wl,-O3 -Wl,--as-needed
LIBS = -lminifb -lX11 -lGL -lpthread -lm

TARGET = main
SRC = main.c load/loadObj.c util/bbox.c object/object.c object/format.c object/scene.c object/material/material.c render/render.c render/cpu/ray.c render/cpu/tile.c render/cpu/font.c render/color/color.c

FLAMEGRAPH_DIR = .flamegraph

TESTS_DIR     = tests
TEST_SRCS     = $(filter-out $(TESTS_DIR)/timings.c, $(wildcard $(TESTS_DIR)/*.c))
TEST_BINS     = $(patsubst $(TESTS_DIR)/%.c, $(TESTS_DIR)/%, $(TEST_SRCS))
TEST_COMMON   = load/loadObj.c util/bbox.c util/threadPool.c util/saveImage.c tests/timings.c object/object.c object/format.c object/scene.c \
                object/material/material.c render/render.c render/cpu/ray.c render/cpu/tile.c \
                render/cpu/font.c render/color/color.c

# Goals passed alongside 'test', e.g. make test testRay → _SPECIFIC = testRay
_SPECIFIC     = $(filter-out test, $(MAKECMDGOALS))
_RUN_TESTS    = $(if $(_SPECIFIC), $(addprefix $(TESTS_DIR)/, $(_SPECIFIC)), $(TEST_BINS))

.PHONY: all clean run flame pgo test $(if $(_SPECIFIC), $(_SPECIFIC))

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

run: $(TARGET)
	./$(TARGET)

# Build rule for any test binary
$(TESTS_DIR)/%: $(TESTS_DIR)/%.c $(TEST_COMMON)
	$(CC) $(CFLAGS) -I$(TESTS_DIR) -o $@ $^ $(LDFLAGS) $(LIBS)

# make test          → build & run all tests
# make test testRay  → build & run only testRay
test: $(_RUN_TESTS)
	@for t in $(_RUN_TESTS); do \
		echo "========================================"; \
		echo "Running: $$t"; \
		echo "========================================"; \
		$$t || exit 1; \
	done

ifneq ($(_SPECIFIC),)
$(_SPECIFIC):
	@:
endif

pgo:
	$(CC) $(CFLAGS) -fno-lto -fprofile-generate -DPGO_MAX_FRAMES=2000 -o $(TARGET)_pgo $(SRC) -L/usr/local/lib -Wl,--gc-sections -Wl,-O3 -Wl,--as-needed $(LIBS)
	./$(TARGET)_pgo
	llvm-profdata-18 merge -output=default.profdata *.profraw
	$(CC) $(CFLAGS) -fprofile-use=default.profdata -fprofile-correction -o $(TARGET) $(SRC) $(LDFLAGS) $(LIBS)
	rm -f $(TARGET)_pgo *.profraw

flame:
	$(CC) -O3 -march=native -fno-omit-frame-pointer -fno-inline-functions -fno-lto \
		-w -I/usr/local/include -Iobject \
		-o $(TARGET) $(SRC) -L/usr/local/lib $(LIBS)
	@if [ ! -d "$(FLAMEGRAPH_DIR)" ]; then \
		echo "Cloning FlameGraph tools..."; \
		git clone --depth=1 https://github.com/brendangregg/FlameGraph $(FLAMEGRAPH_DIR); \
	fi
	sudo perf record -F 99 -g --call-graph fp -o perf.data -- timeout 10 ./$(TARGET) || true
	sudo perf script -i perf.data | $(FLAMEGRAPH_DIR)/stackcollapse-perf.pl | $(FLAMEGRAPH_DIR)/flamegraph.pl > flamegraph.svg
	sudo chown $(USER) perf.data
	@echo "Flame graph saved to flamegraph.svg"

clean:
	rm -f $(TARGET) perf.data flamegraph.svg $(TEST_BINS)
