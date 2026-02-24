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

.PHONY: all clean run flame pgo

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

run: $(TARGET)
	./$(TARGET)

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
	rm -f $(TARGET) perf.data flamegraph.svg
