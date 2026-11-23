# Standalone build for the OpenCL rasterizer experiment. Deliberately separate from the
# root Makefile so nothing here can affect the main binary — the engine sources below are
# reused read-only, and outputs land in ../build/openclRaster (already gitignored).
CC = clang
CFLAGS = -O3 -march=native -mtune=native -flto -ffast-math -funroll-loops -finline-functions
CFLAGS += -fomit-frame-pointer -fno-stack-protector -fdata-sections -ffunction-sections
CFLAGS += -falign-functions=64 -falign-loops=64 -fno-plt -fprefetch-loop-arrays
CFLAGS += -fvectorize -fslp-vectorize -w -I/usr/local/include -I../object

# Pin the system linker: conda's ld on PATH lacks the Debian multiarch search dirs.
LDFLAGS = --ld-path=/usr/bin/ld -flto -Wl,--gc-sections -Wl,-O3 -Wl,--as-needed
LIBS = -lpthread -lm -ljpeg -lOpenCL

BUILD_DIR = ../build/openclRaster

# Engine sources, compiled as-is (same set the root Makefile links for its tests, plus the
# OpenCL helper layer).
ENGINE_SRC = ../load/loadObj.c ../util/bbox.c ../util/threadPool.c ../util/saveImage.c \
	../object/object.c ../object/format.c ../object/scene.c ../object/material/material.c \
	../render/render.c ../render/cpu/ray.c ../render/cpu/ssr.c ../render/cpu/tile.c \
	../render/cpu/font.c ../render/color/color.c ../skybox/skybox.c \
	../render/gpu/format.c

PARITY_SRC = rasterParity.c
RASTER_SRC = raster.c

.PHONY: all run clean

all: $(BUILD_DIR)/rasterParity

$(BUILD_DIR)/rasterParity: $(PARITY_SRC) $(RASTER_SRC) raster.h raster.cl $(ENGINE_SRC)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(PARITY_SRC) $(RASTER_SRC) $(ENGINE_SRC) $(LDFLAGS) $(LIBS)

# The harness loads assets/models and the skybox by relative path, so it runs from the repo root.
run: $(BUILD_DIR)/rasterParity
	cd .. && ./$(BUILD_DIR)/rasterParity

clean:
	rm -f $(BUILD_DIR)/rasterParity
