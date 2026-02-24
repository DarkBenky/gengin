CC = clang
CFLAGS = -O3 -march=native -mtune=native -flto -ffast-math -funroll-loops -finline-functions
CFLAGS += -w -I/usr/local/include -Iobject
LDFLAGS = -flto -L/usr/local/lib
LIBS = -lminifb -lX11 -lGL -lpthread -lm

TARGET = main
SRC = main.c load/loadObj.c util/bbox.c object/object.c object/format.c object/scene.c object/material/material.c render/render.c render/cpu/ray.c render/cpu/tile.c render/cpu/font.c render/color/color.c

# GPU / OpenCL support — enabled automatically when OpenCL headers are present
OPENCL_HEADER := $(shell echo "\#include <CL/cl.h>" | $(CC) -E -x c - 2>/dev/null && echo yes || echo no)
ifeq ($(OPENCL_HEADER),yes)
    CFLAGS += -DUSE_GPU_RASTER
    SRC    += render/gpu/format.c render/gpu/raster.c
    LIBS   += -lOpenCL
endif

FLAMEGRAPH_DIR = .flamegraph

# GPU target — always force OpenCL rasterizer on, deduplicate flags/sources already added by auto-detect
GPU_CFLAGS = $(filter-out -DUSE_GPU_RASTER,$(CFLAGS)) -DUSE_GPU_RASTER
GPU_SRC    = $(filter-out render/gpu/format.c render/gpu/raster.c,$(SRC)) render/gpu/format.c render/gpu/raster.c
GPU_LIBS   = $(filter-out -lOpenCL,$(LIBS)) -lOpenCL

CPU_CFLAGS = $(filter-out -DUSE_GPU_RASTER,$(CFLAGS))
CPU_SRC    = $(filter-out render/gpu/format.c render/gpu/raster.c,$(SRC))
CPU_LIBS   = $(filter-out -lOpenCL,$(LIBS))

.PHONY: all clean run flame gpu

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

gpu: $(GPU_SRC)
	$(CC) $(GPU_CFLAGS) -o $(TARGET) $^ $(LDFLAGS) $(GPU_LIBS)
	./$(TARGET)

run: $(CPU_SRC)
	$(CC) $(CPU_CFLAGS) -o $(TARGET) $^ $(LDFLAGS) $(CPU_LIBS)
	./$(TARGET)

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
