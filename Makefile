CC = clang
CFLAGS = -O3 -march=native -mtune=native -flto -ffast-math -funroll-loops -finline-functions
CFLAGS += -Wall -Wextra -I/usr/local/include
LDFLAGS = -flto -L/usr/local/lib
LIBS = -lminifb -lX11 -lGL -lpthread -lm

TARGET = main
SRC = main.c object/object.c object/format.c object/scene.c render/render.c render/cpu/ray.c

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
