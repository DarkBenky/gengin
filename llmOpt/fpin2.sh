#!/bin/bash
# fpin2.sh "<-D flags>" : build with the exact Makefile flightBench command line
# plus extra defines, run the pinned 20-scenario suite, print the gate lines.
cd /home/user/Desktop/gengin/llmOpt/gengin
CF="-O3 -march=native -mtune=native -flto -ffast-math -funroll-loops -finline-functions -fomit-frame-pointer -fno-stack-protector -fdata-sections -ffunction-sections -falign-functions=64 -falign-loops=64 -fno-plt -fprefetch-loop-arrays -fvectorize -fslp-vectorize -mllvm -polly -w -I/usr/local/include -Iobject -Ideps/minifb/include"
DEFS="$1"
clang $CF $DEFS -DFLIGHT_BENCH -Isimulation -I. -o build/flightBench/flightBench simulation/cSim/flightBench.c simulation/cSim/simulate.c simulation/cSim/import.c simulation/cSim/missile.c simulation/cSim/missileImport.c object/format.c --ld-path=/usr/bin/ld -flto -L/usr/local/lib -Ldeps/minifb/build -Wl,--gc-sections -Wl,-O3 -Wl,--as-needed -lpthread -lm || { echo BUILDFAIL; exit 1; }
./build/flightBench/flightBench | python3 -c '
import sys,json
d=json.loads(sys.stdin.read()); a=d["aggregate"]
base={"miss":79.216,"hitRate":0.600,"tHitMean":25.157,"costUs":399.39}
bt={t["tier"]:t["miss"] for t in [{"tier":"static","miss":76.123},{"tier":"drift","miss":8.231},{"tier":"weave","miss":89.710},{"tier":"step","miss":6.799},{"tier":"jink","miss":215.219}]}
def pc(n,b): return 100.0*(n-b)/b if b else 0.0
print("  miss %.3f (%+.1f%%) hit %.3f (%+.1f%%) tHit %.3f (%+.1f%%) cost %.1f (%+.1f%%) sat %d" % (
  a["miss"],pc(a["miss"],base["miss"]),a["hitRate"],pc(a["hitRate"],base["hitRate"]),
  a["tHitMean"],pc(a["tHitMean"],base["tHitMean"]),a["costUs"],pc(a["costUs"],base["costUs"]),a["satSteps"]))
bad=[]
for t in d["tiers"]:
  dv=pc(t["miss"],bt[t["tier"]])
  print("    %-7s miss %8.3f (%+.1f%%) hit %.2f" % (t["tier"],t["miss"],dv,t["hitRate"]))
  if dv < -10.0: bad.append(t["tier"])
print("  GUARD: miss %s | hit %s | tiers %s" % ("ok" if pc(a["miss"],base["miss"])>1.0 else "FAIL",
  "ok" if a["hitRate"]>=base["hitRate"] else "FAIL", "ok" if not bad else "REGRESSED "+",".join(bad)))
'
