CC = clang
CFLAGS_BASE = -O3 -march=native -mtune=native -flto -ffast-math -funroll-loops -finline-functions
CFLAGS_BASE += -fomit-frame-pointer -fno-stack-protector
CFLAGS_BASE += -fdata-sections -ffunction-sections
CFLAGS_BASE += -falign-functions=64 -falign-loops=64
CFLAGS_BASE += -fno-plt -fprefetch-loop-arrays
CFLAGS_BASE += -fvectorize -fslp-vectorize
CFLAGS_BASE += -mllvm -polly
CFLAGS_BASE += -w -I/usr/local/include -Iobject

# Vendored minifb: headers + locally built static lib (deps/minifb/build)
MINIFB_DIR = deps/minifb
CFLAGS_BASE += -I$(MINIFB_DIR)/include

CFLAGS = $(CFLAGS_BASE)
# Pin the system linker: conda's ld on PATH lacks the Debian multiarch search
# dirs and fails to resolve libxcb/libGL/libc dependencies.
LDFLAGS = --ld-path=/usr/bin/ld -flto -L/usr/local/lib -L$(MINIFB_DIR)/build
LDFLAGS += -Wl,--gc-sections -Wl,-O3 -Wl,--as-needed
LIBS = -lminifb -lxkbcommon -lX11 -lXrandr -lGL -lpthread -lm -ljpeg -lOpenCL

# ---- Output layout: every target builds into its own dir under build/ ----
# build/<target>/<binary> (+ variants like main_debug / main_bench)
# tests -> build/tests, benches -> build/bench, prof -> build/prof, pgo -> build/pgo
BUILD_DIR = build
MAIN_DIR  = $(BUILD_DIR)/main
TEST_DIR  = $(BUILD_DIR)/tests
BENCH_DIR = $(BUILD_DIR)/bench
PROF_DIR  = $(BUILD_DIR)/prof
PGO_DIR   = $(BUILD_DIR)/pgo

# One SVG + perf.data per test, overwritten by the next run of that test
FLAME_TEST_DIR = $(TEST_DIR)/flame

# Auto-use PGO data if available from a previous 'make pgo' run
PROFDATA = $(PGO_DIR)/default.profdata
ifneq ($(wildcard $(PROFDATA)),)
CFLAGS += -fprofile-use=$(PROFDATA) -fprofile-correction
endif

# Bench run length in seconds. Override for a short preflight smoke, e.g.
# `make bench BENCH_DURATION=2`. The full production baseline uses the default.
BENCH_DURATION ?= 10.0

TARGET = $(MAIN_DIR)/main
SRC = main.c client/gameClient.c client/client.c load/loadObj.c util/bbox.c util/threadPool.c object/object.c object/format.c object/scene.c object/material/material.c render/render.c render/cpu/ray.c render/cpu/ssr.c render/cpu/tile.c render/cpu/font.c render/color/color.c skybox/skybox.c keyboar/keyboar.c render/gpu/format.c render/gpu/kernels/cloadrendering/cload.c hexDump/hexDump.c simulation/cSim/import.c simulation/cSim/simulate.c

FLAMEGRAPH_DIR = .flamegraph

TESTS_DIR     = tests
TOOLS_DIR     = tools
TEST_SRCS     = $(filter-out $(TESTS_DIR)/timings.c, $(wildcard $(TESTS_DIR)/*.c))
TEST_BINS     = $(patsubst $(TESTS_DIR)/%.c, $(TEST_DIR)/%, $(TEST_SRCS))
TEST_COMMON   = load/loadObj.c util/bbox.c util/threadPool.c util/saveImage.c tests/timings.c object/object.c object/format.c object/scene.c \
                object/material/material.c render/render.c render/cpu/ray.c render/cpu/ssr.c render/cpu/tile.c \
                render/cpu/font.c render/color/color.c skybox/skybox.c

# Goals passed alongside 'test', e.g. make test testRay → _SPECIFIC = testRay
_SPECIFIC         = $(filter-out build/% tests/% main test all clean debug run flame pgo bench benchUnOpt exampleServer gameServer exampleClient gameClient hexDump train flightController flightController-debug benchFunc testSound testSound3d testRadarScreen, $(MAKECMDGOALS))
_RUN_TESTS        = $(if $(_SPECIFIC), $(addprefix $(TEST_DIR)/, $(_SPECIFIC)), $(TEST_BINS))

# Named tests (make test AOBench) get a perf flame graph; FLAME=1 extends that
# to a plain 'make test' run of everything, FLAME=0 disables it everywhere.
FLAME             ?= 0
_FLAME_ENABLED    = $(if $(filter 0,$(FLAME)),,$(if $(_SPECIFIC),1,$(filter 1,$(FLAME))))
_FLAME_BINS       = $(if $(_FLAME_ENABLED),$(addsuffix _flame,$(_RUN_TESTS)))

BENCH_FUNC_DIR    = bench
BENCH_FUNC_SRCS   = $(wildcard $(BENCH_FUNC_DIR)/*.c)
BENCH_FUNC_BINS   = $(patsubst $(BENCH_FUNC_DIR)/%.c, $(BENCH_DIR)/%, $(BENCH_FUNC_SRCS))
_BENCH_FUNC_SPECIFIC = $(filter-out build/% tests/% bench/% main test all clean debug run flame pgo bench benchUnOpt exampleServer gameServer exampleClient gameClient hexDump train flightController flightController-debug benchFunc testSound testSound3d, $(MAKECMDGOALS))
_RUN_BENCH_FUNCS  = $(if $(_BENCH_FUNC_SPECIFIC), $(addprefix $(BENCH_DIR)/, $(_BENCH_FUNC_SPECIFIC)))

EXAMPLE_SERVER_SRC = server/example.c server/server.c object/format.c
GAME_SERVER_SRC    = server/gameServer.c server/server.c object/format.c
EXAMPLE_CLIENT_SRC = client/example.c client/client.c object/format.c
GAME_CLIENT_SRC    = client/gameClient.c client/client.c object/format.c object/object.c object/scene.c object/material/material.c load/loadObj.c util/bbox.c util/threadPool.c hexDump/hexDump.c
HEX_DUMP_SRC       = hexDump/hexDump.c
TRAIN_SRC          = simulation/cSim/trainNN.c simulation/cSim/dense.c simulation/cSim/simulate.c simulation/cSim/import.c client/client.c util/threadPool.c
FLIGHT_CONTROL_SRC = simulation/cSim/flightControl.c simulation/cSim/simulate.c simulation/cSim/import.c object/format.c
TEST_SOUND_SRC      = sound/soundTest.c
TEST_SOUND3D_SRC    = sound/soundTest3d.c

TEST_RADAR_SCREEN_SRC = radarScreen/testRadarScreen.c util/saveImage.c render/cpu/font.c render/cpu/tile.c

.PHONY: all main clean debug run flame pgo test bench benchUnOpt callgraph perf-report exampleServer gameServer exampleClient gameClient hexDump train flightController flightController-debug benchFunc testSound testSound3d testRadarScreen $(if $(_SPECIFIC), $(_SPECIFIC)) $(if $(_BENCH_FUNC_SPECIFIC), $(_BENCH_FUNC_SPECIFIC))

all: $(TARGET)

# Convenience alias for the main binary (default target)
main: $(TARGET)

$(TARGET): $(SRC)
	@mkdir -p $(MAIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

debug: CFLAGS = -g -O0 -march=native -Wall -I/usr/local/include -Iobject -I$(MINIFB_DIR)/include
debug: $(SRC)
	@mkdir -p $(MAIN_DIR)
	$(CC) $(CFLAGS) -o $(MAIN_DIR)/main_debug $^ $(LDFLAGS) $(LIBS)

exampleServer: $(EXAMPLE_SERVER_SRC)
	@mkdir -p $(BUILD_DIR)/$@
	$(CC) $(CFLAGS_BASE) -o $(BUILD_DIR)/$@/$@ $^ $(LDFLAGS) -lm
	./$(BUILD_DIR)/$@/$@

gameServer: $(GAME_SERVER_SRC)
	@mkdir -p $(BUILD_DIR)/$@
	$(CC) $(CFLAGS_BASE) -o $(BUILD_DIR)/$@/$@ $^ $(LDFLAGS) -lm
	./$(BUILD_DIR)/$@/$@

exampleClient: $(EXAMPLE_CLIENT_SRC)
	@mkdir -p $(BUILD_DIR)/$@
	$(CC) $(CFLAGS_BASE) -o $(BUILD_DIR)/$@/$@ $^ $(LDFLAGS) -lm
	./$(BUILD_DIR)/$@/$@

gameClient: $(GAME_CLIENT_SRC)
	@mkdir -p $(BUILD_DIR)/$@
	$(CC) $(CFLAGS_BASE) -o $(BUILD_DIR)/$@/$@ $^ $(LDFLAGS) -lm
	./$(BUILD_DIR)/$@/$@

hexDump: hexDump/hexDumpTest.c $(HEX_DUMP_SRC)
	@mkdir -p $(BUILD_DIR)/hexDump
	$(CC) $(CFLAGS_BASE) -IhexDump -o $(BUILD_DIR)/hexDump/hexDumpBin $^ $(LDFLAGS) -lm
	./$(BUILD_DIR)/hexDump/hexDumpBin

train: $(TRAIN_SRC)
	cd simulation/cmd && go run .
	@mkdir -p $(BUILD_DIR)/trainNN
	$(CC) $(CFLAGS_BASE) -Isimulation -I. -o $(BUILD_DIR)/trainNN/trainNN $^ $(LDFLAGS) -lm
	./$(BUILD_DIR)/trainNN/trainNN

flightController: $(FLIGHT_CONTROL_SRC)
	@mkdir -p $(BUILD_DIR)/flightController
	$(CC) $(CFLAGS_BASE) -Isimulation -I. -o $(BUILD_DIR)/flightController/flightController $^ $(LDFLAGS) -lm
	./$(BUILD_DIR)/flightController/flightController

flightController-debug: $(FLIGHT_CONTROL_SRC)
	@mkdir -p $(BUILD_DIR)/flightController
	$(CC) -g -O0 -march=native -Wall -Isimulation -I. -Iobject -o $(BUILD_DIR)/flightController/flightController_debug $^ $(LDFLAGS) -lm

testSound: $(TEST_SOUND_SRC)
	@mkdir -p $(BUILD_DIR)/testSound
	$(CC) -O0 -g -Isound -I. -o $(BUILD_DIR)/testSound/testSound $^ $(LDFLAGS) -lSDL3 -lm
	./$(BUILD_DIR)/testSound/testSound

testSound3d: $(TEST_SOUND3D_SRC)
	@mkdir -p $(BUILD_DIR)/testSound3d
	$(CC) -O0 -g -Isound -I. -o $(BUILD_DIR)/testSound3d/testSound3d $^ $(LDFLAGS) -lSDL3 -lm
	./$(BUILD_DIR)/testSound3d/testSound3d

testRadarScreen: $(TEST_RADAR_SCREEN_SRC)
	@mkdir -p $(BUILD_DIR)/testRadarScreen
	$(CC) $(CFLAGS_BASE) -IradarScreen -I. -o $(BUILD_DIR)/testRadarScreen/testRadarScreen $^ $(LDFLAGS) -lm
	./$(BUILD_DIR)/testRadarScreen/testRadarScreen

run: $(TARGET)
	./$(TARGET)

bench: $(SRC)
	@mkdir -p $(MAIN_DIR)
	$(CC) $(CFLAGS) -DBENCH_MODE -DBENCH_DURATION=$(BENCH_DURATION) -o $(MAIN_DIR)/main_bench $^ $(LDFLAGS) $(LIBS)
	./$(MAIN_DIR)/main_bench
	rm -f $(MAIN_DIR)/main_bench

benchUnOpt: $(SRC)
	@mkdir -p $(MAIN_DIR)
	$(CC) -O1 -w -I/usr/local/include -Iobject -I$(MINIFB_DIR)/include -DBENCH_MODE -DBENCH_DURATION=2.0 -o $(MAIN_DIR)/main_bench_unopt $^ -L/usr/local/lib -L$(MINIFB_DIR)/build $(LIBS)
	./$(MAIN_DIR)/main_bench_unopt
	rm -f $(MAIN_DIR)/main_bench_unopt

# Build rule for any test binary
$(TEST_DIR)/%: $(TESTS_DIR)/%.c $(TEST_COMMON)
	@mkdir -p $(TEST_DIR)
	$(CC) $(CFLAGS) -I$(TESTS_DIR) -o $@ $^ $(LDFLAGS) $(LIBS)

# Profiling twin of a test binary: frame pointers kept so perf can unwind the
# stacks and inlining/LTO off so hot functions stay visible. Timings still come
# from the normal binary above, which keeps its aggressive flags.
$(TEST_DIR)/%_flame: $(TESTS_DIR)/%.c $(TEST_COMMON)
	@mkdir -p $(TEST_DIR)
	$(CC) $(CFLAGS_BASE) -fno-omit-frame-pointer -fno-inline-functions -fno-lto -g -I$(TESTS_DIR) \
		-o $@ $^ --ld-path=/usr/bin/ld -L/usr/local/lib -L$(MINIFB_DIR)/build -Wl,--gc-sections -Wl,-O3 -Wl,--as-needed $(LIBS)

# make test          → build & run all tests
# make test testRay  → build & run only testRay (+ perf flame graph)
# make test FLAME=1  → flame graphs for every test as well
test: $(_RUN_TESTS) $(_FLAME_BINS)
	@LOG=$(TEST_DIR)/results.log; \
	> $$LOG; \
	for t in $(_RUN_TESTS); do \
		echo "========================================" | tee -a $$LOG; \
		echo "Running: $$t" | tee -a $$LOG; \
		echo "========================================" | tee -a $$LOG; \
		$$t 2>&1 | tee -a $$LOG || exit 1; \
		if [ -n "$(_FLAME_ENABLED)" ]; then \
			bash $(TOOLS_DIR)/flame.sh $${t}_flame $(FLAME_TEST_DIR)/$$(basename $$t).svg; \
		fi; \
	done

ifneq ($(_SPECIFIC),)
# map bare test goals (make test testRay) onto their build/tests/<name> binary
$(foreach t,$(_SPECIFIC),$(eval $t: $(TEST_DIR)/$t))
endif

# Build rule for any micro-benchmark binary under bench/
$(BENCH_DIR)/%: $(BENCH_FUNC_DIR)/%.c $(TESTS_DIR)/timings.c util/threadPool.c object/format.c
	@mkdir -p $(BENCH_DIR)
	$(CC) $(CFLAGS_BASE) -I$(BENCH_FUNC_DIR) -I$(TESTS_DIR) -o $@ $^ $(LDFLAGS) -lm

# make benchFunc <funcName>  →  build & run bench/<funcName>
benchFunc: $(_RUN_BENCH_FUNCS)
	@if [ -z "$(_BENCH_FUNC_SPECIFIC)" ]; then \
		echo "Usage: make benchFunc <funcName>  (file must exist as bench/<funcName>.c)"; \
	else \
		for t in $(_RUN_BENCH_FUNCS); do \
			echo "========================================"; \
			echo "Running micro-benchmark: $$t"; \
			echo "========================================"; \
			$$t 2>&1; \
		done; \
	fi


pgo:
	@mkdir -p $(PGO_DIR)
	$(CC) $(CFLAGS_BASE) -fno-lto -fprofile-generate -DPGO_MAX_FRAMES=2048 -o $(MAIN_DIR)/main_pgo $(SRC) -L/usr/local/lib -Wl,--gc-sections -Wl,-O3 -Wl,--as-needed $(LIBS)
	LLVM_PROFILE_FILE=$(PGO_DIR)/default_%p.profraw ./$(MAIN_DIR)/main_pgo
	llvm-profdata-18 merge -output=$(PGO_DIR)/default.profdata $(PGO_DIR)/*.profraw
	$(CC) $(CFLAGS_BASE) -fprofile-use=$(PGO_DIR)/default.profdata -fprofile-correction -o $(TARGET) $(SRC) $(LDFLAGS) $(LIBS)
	rm -f $(MAIN_DIR)/main_pgo $(PGO_DIR)/*.profraw

# Profile the app: builds a frame-pointer twin (the shipped binary keeps its
# LTO/inlining flags) and renders flame/icicle/call graph via tools/flame.sh.
FLAME_BIN     = $(PROF_DIR)/main_flame
FLAME_SECONDS = 10

flame:
	@mkdir -p $(PROF_DIR)
	$(CC) -O3 -march=native -fno-omit-frame-pointer -fno-inline-functions -fno-lto -g \
		-w -I/usr/local/include -Iobject -I$(MINIFB_DIR)/include \
		-o $(FLAME_BIN) $(SRC) --ld-path=/usr/bin/ld -L/usr/local/lib -L$(MINIFB_DIR)/build $(LIBS)
	bash $(TOOLS_DIR)/flame.sh $(FLAME_BIN) $(PROF_DIR)/flamegraph.svg $(FLAME_SECONDS)

# Requires: pip install gprof2dot   conda install -c conda-forge graphviz
callgraph:
	@if [ ! -f $(PROF_DIR)/flamegraph.perf.data ]; then echo "No perf data found, run 'make flame' first"; exit 1; fi
	perf script -i $(PROF_DIR)/flamegraph.perf.data | gprof2dot -f perf | dot -Tsvg -o $(PROF_DIR)/flamegraph.callgraph.svg
	@echo "Call graph saved to $(PROF_DIR)/flamegraph.callgraph.svg"

perf-report:
	@if [ ! -f $(PROF_DIR)/flamegraph.perf.data ]; then echo "No perf data found, run 'make flame' first"; exit 1; fi
	perf report -i $(PROF_DIR)/flamegraph.perf.data --no-children

clean:
	rm -rf $(BUILD_DIR)
