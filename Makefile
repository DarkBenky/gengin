CC = clang
CFLAGS_BASE = -O3 -march=native -mtune=native -flto -ffast-math -funroll-loops -finline-functions
CFLAGS_BASE += -fomit-frame-pointer -fno-stack-protector
CFLAGS_BASE += -fdata-sections -ffunction-sections
CFLAGS_BASE += -falign-functions=64 -falign-loops=64
CFLAGS_BASE += -fno-plt -fprefetch-loop-arrays
CFLAGS_BASE += -fvectorize -fslp-vectorize
CFLAGS_BASE += -mllvm -polly
CFLAGS_BASE += -w -I/usr/local/include -Iobject
CFLAGS = $(CFLAGS_BASE)
LDFLAGS = -flto -L/usr/local/lib
LDFLAGS += -Wl,--gc-sections -Wl,-O3 -Wl,--as-needed
LIBS = -lminifb -lX11 -lGL -lpthread -lm -ljpeg -lOpenCL

# Auto-use PGO data if available from a previous 'make pgo' run
PROFDATA = default.profdata
ifneq ($(wildcard $(PROFDATA)),)
CFLAGS += -fprofile-use=$(PROFDATA) -fprofile-correction
endif

TARGET = main
SRC = main.c client/gameClient.c client/client.c load/loadObj.c util/bbox.c util/threadPool.c object/object.c object/format.c object/scene.c object/material/material.c render/render.c render/cpu/ray.c render/cpu/ssr.c render/cpu/tile.c render/cpu/font.c render/color/color.c skybox/skybox.c keyboar/keyboar.c render/gpu/format.c render/gpu/kernels/cloadrendering/cload.c hexDump/hexDump.c

FLAMEGRAPH_DIR = .flamegraph

TESTS_DIR     = tests
TEST_SRCS     = $(filter-out $(TESTS_DIR)/timings.c, $(wildcard $(TESTS_DIR)/*.c))
TEST_BINS     = $(patsubst $(TESTS_DIR)/%.c, $(TESTS_DIR)/%, $(TEST_SRCS))
TEST_COMMON   = load/loadObj.c util/bbox.c util/threadPool.c util/saveImage.c tests/timings.c object/object.c object/format.c object/scene.c \
                object/material/material.c render/render.c render/cpu/ray.c render/cpu/ssr.c render/cpu/tile.c \
                render/cpu/font.c render/color/color.c

# Goals passed alongside 'test', e.g. make test testRay → _SPECIFIC = testRay
_SPECIFIC     = $(filter-out test all clean run flame pgo bench benchUnOpt exampleServer gameServer exampleClient gameClient hexDump train, $(MAKECMDGOALS))
_RUN_TESTS    = $(if $(_SPECIFIC), $(addprefix $(TESTS_DIR)/, $(_SPECIFIC)), $(TEST_BINS))

EXAMPLE_SERVER_SRC = server/example.c server/server.c object/format.c
GAME_SERVER_SRC    = server/gameServer.c server/server.c object/format.c
EXAMPLE_CLIENT_SRC = client/example.c client/client.c object/format.c
GAME_CLIENT_SRC    = client/gameClient.c client/client.c object/format.c object/object.c object/scene.c object/material/material.c load/loadObj.c util/bbox.c util/threadPool.c hexDump/hexDump.c
HEX_DUMP_SRC       = hexDump/hexDump.c
TRAIN_SRC          = simulation/cSim/trainNN.c simulation/cSim/dense.c simulation/cSim/simulate.c simulation/cSim/import.c client/client.c

TRAIN_VIEW_SRC = trainView.c client/gameClient.c client/client.c load/loadObj.c util/bbox.c util/threadPool.c object/object.c object/format.c object/scene.c object/material/material.c render/render.c render/cpu/ray.c render/cpu/ssr.c render/cpu/tile.c render/cpu/font.c render/color/color.c hexDump/hexDump.c

.PHONY: all clean run flame pgo test bench benchUnOpt callgraph perf-report exampleServer gameServer exampleClient gameClient hexDump train trainView $(if $(_SPECIFIC), $(_SPECIFIC))

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

exampleServer: $(EXAMPLE_SERVER_SRC)
	$(CC) $(CFLAGS_BASE) -o $@ $^ $(LDFLAGS) -lm
	./exampleServer

gameServer: $(GAME_SERVER_SRC)
	$(CC) $(CFLAGS_BASE) -o $@ $^ $(LDFLAGS) -lm
	./gameServer

exampleClient: $(EXAMPLE_CLIENT_SRC)
	$(CC) $(CFLAGS_BASE) -o $@ $^ $(LDFLAGS) -lm
	./exampleClient

gameClient: $(GAME_CLIENT_SRC)
	$(CC) $(CFLAGS_BASE) -o $@ $^ $(LDFLAGS) -lm
	./gameClient

hexDump: hexDump/hexDumpTest.c $(HEX_DUMP_SRC)
	$(CC) $(CFLAGS_BASE) -IhexDump -o hexDumpBin $^ $(LDFLAGS) -lm
	./hexDumpBin

train: $(TRAIN_SRC)
	cd simulation/cmd && go run .
	$(CC) $(CFLAGS_BASE) -Isimulation -I. -o simulation/trainNN $^ $(LDFLAGS) -lm
	./simulation/trainNN

trainView: $(TRAIN_VIEW_SRC)
	$(CC) $(CFLAGS_BASE) -o trainView $^ $(LDFLAGS) -lminifb -lX11 -lGL -lpthread -lm -ljpeg
	./trainView

run: $(TARGET)
	./$(TARGET)

bench: $(SRC)
	$(CC) $(CFLAGS) -DBENCH_MODE -DBENCH_DURATION=10.0 -o $(TARGET)_bench $^ $(LDFLAGS) $(LIBS)
	./$(TARGET)_bench
	rm -f $(TARGET)_bench

benchUnOpt: $(SRC)
	$(CC) -O1 -w -I/usr/local/include -Iobject -DBENCH_MODE -DBENCH_DURATION=2.0 -o $(TARGET)_bench_unopt $^ -L/usr/local/lib $(LIBS)
	./$(TARGET)_bench_unopt
	rm -f $(TARGET)_bench_unopt

# Build rule for any test binary
$(TESTS_DIR)/%: $(TESTS_DIR)/%.c $(TEST_COMMON)
	$(CC) $(CFLAGS) -I$(TESTS_DIR) -o $@ $^ $(LDFLAGS) $(LIBS)

# make test          → build & run all tests
# make test testRay  → build & run only testRay
test: $(_RUN_TESTS)
	@LOG=$(TESTS_DIR)/results.log; \
	> $$LOG; \
	for t in $(_RUN_TESTS); do \
		echo "========================================" | tee -a $$LOG; \
		echo "Running: $$t" | tee -a $$LOG; \
		echo "========================================" | tee -a $$LOG; \
		$$t 2>&1 | tee -a $$LOG || exit 1; \
	done

ifneq ($(_SPECIFIC),)
$(_SPECIFIC):
	@:
endif

pgo:
	$(CC) $(CFLAGS_BASE) -fno-lto -fprofile-generate -DPGO_MAX_FRAMES=2048 -o $(TARGET)_pgo $(SRC) -L/usr/local/lib -Wl,--gc-sections -Wl,-O3 -Wl,--as-needed $(LIBS)
	./$(TARGET)_pgo
	llvm-profdata-18 merge -output=default.profdata *.profraw
	$(CC) $(CFLAGS_BASE) -fprofile-use=default.profdata -fprofile-correction -o $(TARGET) $(SRC) $(LDFLAGS) $(LIBS)
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
	sudo perf script -i perf.data | gprof2dot -f perf | dot -Tsvg -o callgraph.svg
	sudo chown $(USER) perf.data perf.data.old callgraph.svg 2>/dev/null || true
	@echo "Flame graph saved to flamegraph.svg"
	@echo "Call graph saved to callgraph.svg"

# Requires: pip install gprof2dot   apt install graphviz
callgraph:
	@if [ ! -f perf.data ]; then echo "No perf.data found, run 'make flame' first"; exit 1; fi
	sudo perf script -i perf.data | gprof2dot -f perf | dot -Tsvg -o callgraph.svg
	sudo chown $(USER) callgraph.svg 2>/dev/null || true
	@echo "Call graph saved to callgraph.svg"

perf-report:
	@if [ ! -f perf.data ]; then echo "No perf.data found, run 'make flame' first"; exit 1; fi
	sudo perf report -i perf.data --no-children

clean:
	rm -f $(TARGET) perf.data flamegraph.svg callgraph.svg $(TEST_BINS) $(TARGET)_bench $(TARGET)_bench_unopt bench_results.json trainView
