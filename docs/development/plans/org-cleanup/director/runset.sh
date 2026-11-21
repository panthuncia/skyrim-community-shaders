#!/usr/bin/env bash
# runset.sh <label> <cfg>...  runs each config once, one summary line each; stops after two failures in a row
cd /c/Users/matth/source/repos/SARP/external_refs/cs-org-dxvk
label=$1; shift; bad=0
for c in "$@"; do
  line=$(CS_TEST_BACKGROUND=1 DXVK_DEBUG=hang timeout 1800 python tools/cs-test/cs_test.py run $c --stage nfg --wait 3600 --label $label 2>&1 | tail -1)
  echo "$line"
  if echo "$line" | grep -q ": passed"; then bad=0; else bad=$((bad+1)); fi
  if [ $bad -ge 2 ]; then echo "STOPPED after two failures"; taskkill //F //IM SkyrimSE.exe >/dev/null 2>&1; taskkill //F //IM ModOrganizer.exe >/dev/null 2>&1; python tools/cs-test/cs_test.py restore >/dev/null 2>&1; exit 1; fi
done
