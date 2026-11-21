#!/usr/bin/env bash
# mkwt.sh <part> [base]  -> worktree ../org-port-worktrees/simplify-<part>, branch org-native/simplify-<part>
set -e
M=/c/Users/matth/source/repos/SARP/external_refs/cs-org-dxvk
W=/c/Users/matth/source/repos/SARP/external_refs/org-port-worktrees/simplify-$1
base=${2:-org_integration}
git -C $M worktree add -q "$W" -b org-native/simplify-$1 $base
bash "$(dirname "$0")/init-submodules-local.sh" "$W" "$M" 2>&1 | grep -v '^skip' || true
echo "ready $W"
