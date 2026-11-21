#!/usr/bin/env bash
# Usage: bstage.sh   (from anywhere) builds the main checkout with build-devfast.cmd and stages nfg only if the build succeeded.
# Build Dev-Fast; stage only if the build succeeds. Prints the errors otherwise.
cd /c/Users/matth/source/repos/SARP/external_refs/cs-org-dxvk
log=${TMP:-/tmp}/cs-devfast-build.log
cmd //c "docs\development\plans\org-cleanup\director\build-devfast.cmd" > $log 2>&1
if grep -q -E "FAILED:|error C|error LNK|ninja: build stopped" $log; then
  grep -E "error C|error LNK|fatal error" $log | sort -u | cut -c1-300 | head -40
  echo BUILD-FAILED; exit 1
fi
python tools/cs-test/cs_test.py stage . --name nfg --preset Dev-Fast --base-aio "C:/Modding/MO2/mods/CS AB nfg" 2>&1 | tail -1
