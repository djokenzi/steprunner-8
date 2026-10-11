#!/usr/bin/env bash
# Hide / show layout lines that get in the way while editing layout.conf in Skin Studio (v1.97.0).
#   ./skin_edit.sh hide   comment them out:  the full-screen st_cond__open catcher (it takes every click on the
#                         first tab) and the RND / CLR buttons of the left section (lbtn: ORANGE, TEAL, VIOLET, CLR)
#   ./skin_edit.sh show   put them back exactly as they were
#   ./skin_edit.sh status how many lines are hidden now
# The lines get the prefix "#EDIT-HIDDEN# " (a comment: Skin Studio keeps it when it saves). build.sh refuses to build
# while any line is hidden, so the skin never ships without them. Close the layout in Skin Studio before hide / show
# (or reload it after), or Studio's next save writes its old copy back.
set -euo pipefail
cd "$(dirname "$0")"
M='#EDIT-HIDDEN# '
case "${1:-}" in
  hide)
    python3 - "$M" <<'PY'
import re, sys
M = sys.argv[1]
lines = open('layout.conf').read().split('\n')
n = 0
for i, l in enumerate(lines):
    if l.startswith('#'): continue
    if re.search(r'\bkey=st_cond__open\b', l) or re.search(r'\bwhen=lbtn:(ORANGE|TEAL|VIOLET|CLR)\b', l):
        lines[i] = M + l; n += 1
open('layout.conf', 'w').write('\n'.join(lines))
print('hidden: %d lines' % n)
PY
    ;;
  show)
    n=$(grep -c "^$M" layout.conf || true)
    sed -i "s/^$M//" layout.conf
    echo "shown again: $n lines" ;;
  status)
    echo "hidden now: $(grep -c "^$M" layout.conf || true) lines" ;;
  *) echo "usage: $0 hide|show|status" >&2; exit 1 ;;
esac
