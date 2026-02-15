CC = clang
CFLAGS = -O3 -march=native -mtune=native -flto -ffast-math -funroll-loops -finline-functions
CFLAGS += -Wall -Wextra -I/usr/local/include
LDFLAGS = -flto -L/usr/local/lib
LIBS = -lminifb -lX11 -lGL -lpthread -lm
GPU_LIBS = -lOpenCL

TARGET = main
SRC = main.c object/object.c object/format.c render/render.c
GPU_SRC = render/renderGpu.c
GPU_TARGET = main_gpu

.PHONY: all clean run run_gpu gpu-test

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

$(GPU_TARGET): $(SRC) $(GPU_SRC)
	$(CC) $(CFLAGS) -DUSE_GPU -o $@ $^ $(LDFLAGS) $(LIBS) $(GPU_LIBS)

run: $(TARGET)
	./$(TARGET)

run_gpu: $(GPU_TARGET)
	./$(GPU_TARGET)

clean:
	rm -f $(TARGET) $(GPU_TARGET)

gpu-test:
	$(CC) $(CFLAGS) -c $(GPU_SRC)
