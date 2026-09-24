#!/bin/bash
# Build the bench binary with the exact production bench flags + -g, so perf can
# attribute samples through inlining (the `make flame` build uses
# -fno-inline-functions and misattributes inline math to out-of-line symbols).
set -e
cd /root/gengin/llmOpt/gengin
mkdir -p build/ab
CMD=$(make -n bench 2>/dev/null | sed -n '2p')
# swap the output path and append debug info
CMD=$(echo "$CMD" | sed 's#-o build/main/main_bench#-g -fno-omit-frame-pointer -o build/ab/prof_bench#')
echo "$CMD"
eval "$CMD"
echo "built build/ab/prof_bench"
