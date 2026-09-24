#!/bin/bash
# Paired A/B isolating ONE file on top of the already-applied BVH change:
#   base_sah_bench = current tree with skybox/skybox.c stashed (BVH SAH only)
#   mod_sky_bench  = current tree (BVH SAH + skybox shared reciprocal)
# Both are built with the identical command line, so they differ only in skybox.c.
set -e
cd /root/gengin/llmOpt/gengin
mkdir -p build/ab

CMD=$(make -n bench 2>/dev/null | sed -n '2p')
MODCMD=$(echo "$CMD" | sed 's#-o build/main/main_bench#-o build/ab/mod_sky_bench#')
BASECMD=$(echo "$CMD" | sed 's#-o build/main/main_bench#-o build/ab/base_sah_bench#')

echo "--- building mod_sky_bench (current tree)"
eval "$MODCMD"

echo "--- stashing skybox/skybox.c to build base_sah_bench"
git stash push -- skybox/skybox.c >/dev/null
trap 'git stash pop >/dev/null 2>&1 || true' EXIT
eval "$BASECMD"
git stash pop >/dev/null
trap - EXIT

ls -la build/ab/base_sah_bench build/ab/mod_sky_bench
echo "--- skybox.c diff restored?"
git diff --stat -- skybox/skybox.c
