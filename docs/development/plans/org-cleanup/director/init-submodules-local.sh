#!/usr/bin/env bash
# Initialise every submodule of worktree $1 from the matching checkout under $2, recursively.
# Several pinned submodule commits exist only locally, so commits are fetched from the local checkout.
set -u
dst="$1"; src="$2"
git -C "$dst" config -f .gitmodules --get-regexp '^submodule\..*\.path$' 2>/dev/null | while read key path; do
  name="${key#submodule.}"; name="${name%.path}"
  [ -e "$src/$path/.git" ] || { echo "skip $dst/$path (no local source)"; continue; }
  sha=$(git -C "$dst" ls-tree HEAD "$path" | awk '{print $3}')
  if [ ! -e "$dst/$path/.git" ]; then
    git -C "$dst" -c protocol.file.allow=always -c "submodule.$name.url=$src/$path" -c "submodule.$name.update=checkout" \
      submodule update --init --quiet -- "$path" 2>/dev/null
  fi
  if [ "$(git -C "$dst/$path" rev-parse HEAD 2>/dev/null)" != "$sha" ]; then
    git -C "$dst/$path" fetch -q "$src/$path" "$sha" && git -C "$dst/$path" checkout -q "$sha" || echo "FAILED $dst/$path"
  fi
  "$0" "$dst/$path" "$src/$path"
done
