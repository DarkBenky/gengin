#!/bin/bash
# usage: fsw.sh "<-D flags>" [seedsPerTier]
# Builds flightBench + FlightSeedProbe with the Makefile codegen plus extra
# defines, runs the pinned 20-scenario suite and the N-seed probe, prints both
# against the baseline pinned at 2452d0f on this host.
cd /home/user/Desktop/gengin/llmOpt/gengin || exit 1
CF="${CF_OVERRIDE:-"-O3 -march=native -mtune=native -flto -ffast-math -funroll-loops -finline-functions -fomit-frame-pointer -fdata-sections -ffunction-sections -falign-functions=64 -falign-loops=64 -fno-plt -fprefetch-loop-arrays -fvectorize -fslp-vectorize -mllvm -polly -w -I/usr/local/include -Iobject -Ideps/minifb/include"}"
DEFS="$1"
SEEDS="${2:-120}"
mkdir -p build/flightBench build/bench
rm -f build/flightBench/flightBench build/bench/FlightSeedProbe
clang $CF $DEFS -DFLIGHT_BENCH -Isimulation -I. -o build/flightBench/flightBench simulation/cSim/flightBench.c simulation/cSim/simulate.c simulation/cSim/import.c simulation/cSim/missile.c simulation/cSim/missileImport.c object/format.c --ld-path=/usr/bin/ld -flto -L/usr/local/lib -Ldeps/minifb/build -Wl,--gc-sections -Wl,-O3 -Wl,--as-needed -lpthread -lm || { echo BUILDFAIL; exit 1; }
clang $CF $DEFS -Ibench -Itests -o build/bench/FlightSeedProbe bench/FlightSeedProbe.c tests/timings.c util/threadPool.c object/format.c --ld-path=/usr/bin/ld -flto -L/usr/local/lib -Ldeps/minifb/build -Wl,--gc-sections -Wl,-O3 -Wl,--as-needed -lm || { echo BUILDFAIL; exit 1; }
echo "== $DEFS  (seeds/tier=$SEEDS)"
./build/flightBench/flightBench | python3 -c '
import sys,json
d=json.loads(sys.stdin.read()); a=d["aggregate"]
base={"miss":66.784,"hitRate":0.600,"tHitMean":25.956,"costUs":406.72,"effort":6.0586}
bt={"static":1.105,"drift":17.218,"weave":90.224,"step":10.557,"jink":214.816}
def pc(n,b): return 100.0*(n-b)/b if b else 0.0
print("  pinned miss %.3f (%+.1f%%) hit %.3f (%+.1f%%) tHit %.3f (%+.1f%%) effort %.3f (%+.1f%%) cost %.1f (%+.1f%%) sat %d" % (
  a["miss"],pc(a["miss"],base["miss"]),a["hitRate"],pc(a["hitRate"],base["hitRate"]),
  a["tHitMean"],pc(a["tHitMean"],base["tHitMean"]),a["effort"],pc(a["effort"],base["effort"]),
  a["costUs"],pc(a["costUs"],base["costUs"]),a["satSteps"]))
bad=[]
for t in d["tiers"]:
  dv=pc(t["miss"],bt[t["tier"]])
  print("    %-7s miss %8.3f (%+.1f%%) hit %.2f" % (t["tier"],t["miss"],dv,t["hitRate"]))
  if dv > 10.0: bad.append(t["tier"])
print("  GUARD: miss %s | hit %s | tHit %s | cost %s | tiers %s" % (
  "ok" if pc(a["miss"],base["miss"])<-1.0 else "FAIL",
  "ok" if pc(a["hitRate"],base["hitRate"])>=-0.0 else "FAIL",
  "ok" if pc(a["tHitMean"],base["tHitMean"])<5.0 else "FAIL",
  "ok" if pc(a["costUs"],base["costUs"])<20.0 else "FAIL",
  "ok" if not bad else "REGRESSED "+",".join(bad)))
'
./build/bench/FlightSeedProbe $SEEDS | python3 -c '
import sys
lines=sys.stdin.read().strip().split("\n")
print("  probe "+lines[1].strip())
for l in lines[2:]: print("   "+l.strip())
'
