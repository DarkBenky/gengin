#!/bin/bash
# Paired A/B bench harness: build two variants, alternate runs in one window.
set -e
cd /home/user/Desktop/gengin/llmOpt/gengin

SRC="main.c client/gameClient.c client/client.c load/loadObj.c util/bbox.c util/threadPool.c object/object.c object/format.c object/scene.c object/material/material.c render/render.c render/cpu/ray.c render/cpu/ssr.c render/cpu/tile.c render/cpu/font.c render/color/color.c skybox/skybox.c keyboard/keyboard.c render/gpu/format.c render/gpu/kernels/cloadrendering/cload.c hexDump/hexDump.c simulation/cSim/import.c simulation/cSim/simulate.c"
CFLAGS="-O3 -march=native -mtune=native -flto -ffast-math -funroll-loops -finline-functions -fomit-frame-pointer -fno-stack-protector -fdata-sections -ffunction-sections -falign-functions=64 -falign-loops=64 -fno-plt -fprefetch-loop-arrays -fvectorize -fslp-vectorize -mllvm -polly -w -I/usr/local/include -Iobject -Ideps/minifb/include"
LDFLAGS="--ld-path=/usr/bin/ld -flto -L/usr/local/lib -Ldeps/minifb/build -Wl,--gc-sections -Wl,-O3 -Wl,--as-needed -lminifb -lxkbcommon -lX11 -lXrandr -lGL -lpthread -lm -ljpeg -lOpenCL"
DUR=${DUR:-6.0}

build() { # $1 = out name
  clang $CFLAGS -DBENCH_MODE -DBENCH_DURATION=$DUR -o build/main/$1 $SRC $LDFLAGS
}

build main_B
git stash push -q
build main_A
git stash pop -q

for i in $(seq 1 ${PAIRS:-5}); do
  for v in main_A main_B; do
    out=$(./build/main/$v 2>/dev/null | grep -E "^  (avg|median)")
    avg=$(echo "$out" | awk '/avg/{print $3}')
    med=$(echo "$out" | awk '/median/{print $3}')
    echo "pair $i $v avg=$avg median=$med"
  done
done
