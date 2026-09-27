# System toolchain on purpose: conda's compiler injects an RPATH to conda libs whose
# libOpenCL/libcuda stubs break the ICD loader (no platforms, segfaults). The RPATH
# below (old-style DT_RPATH, searched before LD_LIBRARY_PATH) pins the system OpenCL.
CC = /usr/bin/clang
CFLAGS = -O3 -Wall -march=native
LDFLAGS = -Wl,--disable-new-dtags -Wl,-rpath,/usr/lib/x86_64-linux-gnu --ld-path=/usr/bin/ld

mnist: main.c loadMNIST.h ../../render/gpu/format.c ../kernelGen.h ../ccnKernel2d.cl
	$(CC) $(CFLAGS) main.c ../../render/gpu/format.c -o mnist -L/usr/lib/x86_64-linux-gnu $(LDFLAGS) -lOpenCL -lm

run: mnist
	./mnist

clean:
	rm -f mnist
