#!/bin/bash
# Build the ath12k board file for the NP750XQA's WCN7850.
#
# linux-firmware's board-2.bin has no entry for this board name ("failed to fetch
# board data"). The generic WCN7850 entry shipped for the Lenovo ThinkPad T14s
# (subsystem e0e6) works here unchanged, so copy it under our name. The result is
# a modified linux-firmware file, which its licence does not allow redistributing.
#
#   usage: make-board-2.sh [stock board-2.bin or .zst]  -> ./board-2.bin
#   install: cp board-2.bin /usr/lib/firmware/ath12k/WCN7850/hw2.0/board-2.bin
#            (the kernel prefers the uncompressed file over board-2.bin.zst)
set -euo pipefail
# Prefer the .zst: on a machine that already ran this script, the plain file is
# the modified one, and the .zst is the pristine copy pacman keeps.
STOCK=${1:-/usr/lib/firmware/ath12k/WCN7850/hw2.0/board-2.bin.zst}
[ -f "$STOCK" ] || STOCK="${STOCK%.zst}"
[ -f "$STOCK" ] || { echo "stock board-2.bin not found (install linux-firmware)"; exit 1; }
NAME='bus=pci,vendor=17cb,device=1107,subsystem-vendor=17cb,subsystem-device=1107,qmi-chip-id=2,qmi-board-id=255'
DONOR='subsystem-device=e0e6,qmi-chip-id=2,qmi-board-id=255'
ENC=$(cd "$(dirname "$0")" && pwd)/ath12k-bdencoder
OUT=$PWD/board-2.bin
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
case "$STOCK" in *.zst) zstd -dq "$STOCK" -o "$W/stock.bin";; *) cp "$STOCK" "$W/stock.bin";; esac
cd "$W"
python3 "$ENC" -e stock.bin > /dev/null
python3 - "$NAME" "$DONOR" <<'PY'
import json, shutil, sys
name, donor = sys.argv[1], sys.argv[2]
top = json.load(open('board-2.json')); board = top[0]['board']
if any(name in e['names'] for e in board):
    print('stock file already has our entry; nothing to do'); sys.exit(0)
src = [e for e in board if any(n.endswith(donor) for n in e['names'])]
if not src: sys.exit('donor entry %s not found in stock board-2.bin' % donor)
shutil.copy(src[0]['data'], 'generic.bin')
board.append({'names': [name], 'data': 'generic.bin'})
json.dump(top, open('board-2.json', 'w'), indent=1)
PY
python3 "$ENC" -c board-2.json > /dev/null
cp board-2.bin "$OUT"
echo "wrote $OUT ($(stat -c %s "$OUT") bytes, sha256 $(sha256sum "$OUT" | cut -c1-16)...)"
