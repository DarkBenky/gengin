#!/bin/bash
# usage: fsweep.sh "<-D flags>" [seeds]
cd /home/user/Desktop/gengin/llmOpt/gengin
CF="-O3 -march=native -mtune=native -flto -ffast-math -funroll-loops -finline-functions -fomit-frame-pointer -fno-stack-protector -fdata-sections -ffunction-sections -falign-functions=64 -falign-loops=64 -fno-plt -fprefetch-loop-arrays -fvectorize -fslp-vectorize -mllvm -polly -w -I/usr/local/include -Iobject"
DEFS="$1"
SEEDS="${2:-120}"
clang $CF $DEFS -DFLIGHT_BENCH -Isimulation -I. -o build/flightBench/flightBench simulation/cSim/flightBench.c simulation/cSim/simulate.c simulation/cSim/import.c simulation/cSim/missile.c simulation/cSim/missileImport.c object/format.c --ld-path=/usr/bin/ld -flto -L/usr/local/lib -Wl,--gc-sections -lpthread -lm || { echo BUILDFAIL; exit 1; }
clang $CF $DEFS -Ibench -Itests -o build/bench/FlightSeedProbe bench/FlightSeedProbe.c tests/timings.c util/threadPool.c object/format.c --ld-path=/usr/bin/ld -flto -L/usr/local/lib -Ldeps/minifb/build -Wl,--gc-sections -Wl,-O3 -Wl,--as-needed -lm || { echo BUILDFAIL; exit 1; }
echo "== $DEFS"
./build/flightBench/flightBench | python3 -c '
import sys,json,re
s=sys.stdin.read()
d=json.loads(s)
a=d["aggregate"]
print("  pinned miss %.3f hit %.3f effort %.4f | %s" % (a["miss"],a["hitRate"],a["effort"],
  " ".join("%s %.1f/%.2f"%(t["tier"],t["miss"],t["hitRate"]) for t in d["tiers"])))
'
./build/bench/FlightSeedProbe $SEEDS | tail -6 | tr '\n' ' '
echo
