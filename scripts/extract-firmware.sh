#!/bin/bash
# Copy the NP750XQA's signed Qualcomm firmware out of a Windows installation.
#
# Five files are signed for this board and exist only in Samsung's Windows driver
# packages: the audio DSP (also runs the battery/charger service), the compute DSP,
# their DT blobs, and the GPU zap shader. Without them there is no audio, no battery
# state and no GPU. They are not in linux-firmware and are not redistributed here.
#
#   usage: extract-firmware.sh <Windows root or DriverStore dir> [firmware dir]
#   e.g.   sudo extract-firmware.sh /mnt/windows
#          sudo extract-firmware.sh /mnt/windows/Windows/System32/DriverStore/FileRepository
#
# Files are matched by sha256 against the set this port was verified with. A file
# that is present but different (a newer driver package) is still installed, with a
# warning, since Samsung ships updates.
set -euo pipefail
SRC=${1:?path to the Windows installation or its DriverStore}
DST=${2:-/usr/lib/firmware}
OUT=$DST/qcom/x1p42100/SAMSUNG/NP750XQA
declare -A WANT=(
  [qcadsp8380.mbn]=b38997ed85148c2431cddf779b03589091be20cc94f2ed832b8bd6f39f466f1a
  [qccdsp8380.mbn]=4a67a03367f2eff2f8a0e867ca25d2bf2fcd5aee3e41e2c9f436c804e257c789
  [qcdxkmsucpurwa.mbn]=dbab90d2ce98dc49e57e3a7f72dd4847d11297642381b2e030f30f7701d954b2
  [adsp_dtbs.elf]=e5749363f0058c94f7efe46a4b0aed9d5d9541398870de979cd9a92b820347f9
  [cdsp_dtbs.elf]=93941f040da14b8305d39579686d886706d22954a538b03da676c1aaa191797f
)
mkdir -p "$OUT"
missing=0
for name in "${!WANT[@]}"; do
  mapfile -t cands < <(find "$SRC" -iname "$name" -type f 2>/dev/null)
  if [ ${#cands[@]} -eq 0 ]; then echo "MISSING  $name"; missing=1; continue; fi
  pick=""
  for c in "${cands[@]}"; do
    [ "$(sha256sum "$c" | cut -d' ' -f1)" = "${WANT[$name]}" ] && { pick=$c; break; }
  done
  if [ -n "$pick" ]; then
    echo "ok       $name  (verified)"
  else
    pick=$(ls -S "${cands[@]}" | head -1)
    echo "WARNING  $name  no copy matches the verified hash; installing the largest of ${#cands[@]} found: $pick"
  fi
  install -m644 "$pick" "$OUT/$name"
done
# The pd-mapper service files are identical to linux-firmware's x1e80100 copies.
for j in adspr adsps adspua cdspr battmgr; do
  s=$DST/qcom/x1e80100/$j.jsn
  [ -f "$s" ] || { echo "MISSING  $j.jsn (install linux-firmware)"; missing=1; continue; }
  install -m644 "$s" "$OUT/$j.jsn"
done
echo "firmware in $OUT"
[ $missing -eq 0 ] || { echo "some files are missing; the initramfs will not have them"; exit 1; }
echo "Wi-Fi: build the board file with scripts/make-board-2.sh. Then rebuild the initramfs."
