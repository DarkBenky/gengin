#!/bin/bash
# Build build/ab/base_bench (pristine object.c) and build/ab/mod_bench (current
# tree) with the SAME command line, so the two binaries differ only in the source
# change under test.
set -e
cd /root/gengin/llmOpt/gengin
mkdir -p build/ab

CMD=$(make -n bench 2>/dev/null | sed -n '2p')
MODCMD=$(echo "$CMD" | sed 's#-o build/main/main_bench#-o build/ab/mod_bench#')
BASECMD=$(echo "$CMD" | sed 's#-o build/main/main_bench#-o build/ab/base_bench#')

DIRTY=$(git diff --name-only)
if [ -z "$DIRTY" ]; then
  echo "ERROR: tree is clean — nothing to test" >&2
  exit 1
fi
echo "modified tracked files: $DIRTY"

echo "--- building mod_bench"
eval "$MODCMD"

echo "--- stashing $DIRTY to build base_bench"
git stash push -- $DIRTY >/dev/null
trap 'git stash pop >/dev/null 2>&1 || true' EXIT
eval "$BASECMD"
git stash pop >/dev/null
trap - EXIT

ls -la build/ab/base_bench build/ab/mod_bench
git diff --stat
