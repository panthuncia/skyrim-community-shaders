#!/bin/bash
# Runs cs-test runs with Post Processing mode on; restores SettingsUser.json after.
# Usage: ppbatch.sh "<cfg> <label> [--set ...]" ...
cd /c/Users/matth/source/repos/SARP/external_refs/cs-org-dxvk
export CS_TEST_BACKGROUND=1 DXVK_DEBUG=hang
S=/c/Modding/MO2/overwrite/SKSE/Plugins/CommunityShaders/SettingsUser.json
B="${PP_BACKUP:-$HOME/SettingsUser.backup.json}"  # make it first: cp "$S" "$B"
cmp "$S" "$B" || { echo "settings differ from backup; stop"; exit 1; }
trap 'cp "$B" "$S"; echo restored' EXIT
python - "$S" <<'EOF'
import json,sys
p=sys.argv[1]; d=json.load(open(p,encoding='utf-8'))
def walk(o):
    if isinstance(o,dict):
        for k,v in list(o.items()):
            if k=="Post Processing Mode": o[k]="PostProcessing"
            elif k=="Disable at Boot" and isinstance(v,dict) and "PostProcessing" in v: v["PostProcessing"]=False
            else: walk(v)
    elif isinstance(o,list):
        for v in o: walk(v)
walk(d); json.dump(d,open(p,'w',encoding='utf-8'),indent=4); print("mode set")
EOF
for spec in "$@"; do
  set -- $spec
  cfg=$1; label=$2; shift 2
  python tools/cs-test/cs_test.py run "$cfg" --stage nfg --set features.PostProcessing=true --label "$label" "$@" 2>&1 | grep -E "^\S+ \[" | tail -1
done
