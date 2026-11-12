#!/bin/bash
cd /home/user/Desktop/gengin/llmOpt/gengin
CF="-O3 -march=native -mtune=native -flto -ffast-math -funroll-loops -finline-functions -fomit-frame-pointer -fno-stack-protector -fdata-sections -ffunction-sections -falign-functions=64 -falign-loops=64 -fno-plt -fprefetch-loop-arrays -fvectorize -fslp-vectorize -mllvm -polly -w -I/usr/local/include -Iobject"
DEFS="$1"
clang $CF $DEFS -DFLIGHT_BENCH -Isimulation -I. -o build/flightBench/flightBench simulation/cSim/flightBench.c simulation/cSim/simulate.c simulation/cSim/import.c simulation/cSim/missile.c simulation/cSim/missileImport.c object/format.c --ld-path=/usr/bin/ld -flto -L/usr/local/lib -Wl,--gc-sections -lpthread -lm || { echo BUILDFAIL; exit 1; }
printf "%-70s " "$DEFS"
./build/flightBench/flightBench | python3 -c '
import sys,json
d=json.loads(sys.stdin.read())
a=d["aggregate"]
print("pinned miss %.3f hit %.3f | %s" % (a["miss"],a["hitRate"],
  " ".join("%s %.1f/%.2f"%(t["tier"],t["miss"],t["hitRate"]) for t in d["tiers"])))
'
