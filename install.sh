#!/bin/bash
# Install the firmware and the userspace side of the port: charging, memory, audio,
# camera, udev, module lists. Idempotent. Kernel, DTB and initramfs are not touched; see README.
#   sudo ./install.sh            install and enable
#   DESTDIR=/tmp/x ./install.sh  copy files only (no systemctl, no sysctl)
set -euo pipefail
S=$(cd "$(dirname "$0")" && pwd)/userspace
D=${DESTDIR:-}
[ -n "$D" ] || [ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
put(){ install -D -m "$1" "$S/$2" "$D$3"; echo "  $3"; }

echo "firmware:"
FW=$(dirname "$S")/firmware/qcom/x1p42100/SAMSUNG/NP750XQA
for f in "$FW"/*; do install -D -m644 "$f" "$D/usr/lib/firmware/qcom/x1p42100/SAMSUNG/NP750XQA/$(basename "$f")"; done
echo "  /usr/lib/firmware/qcom/x1p42100/SAMSUNG/NP750XQA/ ($(ls "$FW" | wc -l) files)"
if [ -z "$D" ] && [ ! -f /usr/lib/firmware/ath12k/WCN7850/hw2.0/board-2.bin ]; then
  echo "  Wi-Fi board file: run scripts/make-board-2.sh and copy board-2.bin to /usr/lib/firmware/ath12k/WCN7850/hw2.0/"
fi
echo "charging:"
put 755 charging/book4-pd-charge            /usr/local/bin/book4-pd-charge
put 644 charging/book4-pd-charge.service    /etc/systemd/system/book4-pd-charge.service
echo "memory:"
put 755 memory/zram-swap                    /usr/local/bin/zram-swap
put 644 memory/zram-swap.service            /etc/systemd/system/zram-swap.service
put 644 memory/99-zram.conf                 /etc/sysctl.d/99-zram.conf
put 644 memory/earlyoom                     /etc/default/earlyoom
echo "audio: ALSA UCM profile"
U=/usr/share/alsa/ucm2/conf.d/x1e80100
put 644 audio/ucm2/Samsung-GalaxyBook4Edge.conf      $U/Samsung-GalaxyBook4Edge.conf
put 644 audio/ucm2/Samsung-GalaxyBook4Edge-HiFi.conf $U/Samsung-GalaxyBook4Edge-HiFi.conf
# UCM is selected by the card's long name, which carries the SKU suffix (e.g.
# ...-NP750XQA_KB2UK), so the include file is named after the running card.
LN=$(awk 'f{sub(/^ +/,""); print; exit} /^ *0 \[/{f=1}' /proc/asound/cards 2>/dev/null || true)
if [ -n "$LN" ]; then
  install -D -m644 "$S/audio/ucm2/Samsung-GalaxyBook4Edge.conf" "$D$U/$LN.conf"; echo "  $U/$LN.conf"
else
  echo "  (no sound card yet: after the first boot with the port's DTB, run install.sh again for the UCM long-name file)"
fi
echo "network:"
put 644 network/iwd-main.conf               /etc/iwd/main.conf
put 644 network/wifi-backend.conf           /etc/NetworkManager/conf.d/wifi-backend.conf
echo "audio (per-user PipeWire/WirePlumber config for the invoking user):"
U=${SUDO_USER:-$USER}; H=$(getent passwd "$U" | cut -d: -f6)
for f in 61-book4-speaker-eq.conf 62-book4-echo-cancel.conf 60-book4-mic-lowpass.conf.disabled; do
  install -D -m644 -o "$U" "$S/audio/$f" "$D$H/.config/pipewire/pipewire.conf.d/$f"; echo "  ~/.config/pipewire/pipewire.conf.d/$f"
done
install -D -m644 -o "$U" "$S/audio/51-book4-speaker-position.conf" "$D$H/.config/wireplumber/wireplumber.conf.d/51-book4-speaker-position.conf"
echo "  ~/.config/wireplumber/wireplumber.conf.d/51-book4-speaker-position.conf"
echo "camera:"
install -D -m644 -o "$U" "$S/camera/52-book4-hide-raw-camss.conf" "$D$H/.config/wireplumber/wireplumber.conf.d/52-book4-hide-raw-camss.conf"
echo "  ~/.config/wireplumber/wireplumber.conf.d/52-book4-hide-raw-camss.conf"
install -D -m644 -o "$U" "$S/camera/configuration.yaml" "$D$H/.config/libcamera/configuration.yaml"
echo "  ~/.config/libcamera/configuration.yaml"
put 644 camera/ov02c10.yaml                 /usr/share/libcamera/ipa/simple/ov02c10.yaml
echo "  (libcamera itself: build userspace/camera/libcamera/PKGBUILD with makepkg and install all three packages)"
echo "udev + modules:"
put 644 udev/90-book4-gpu-floor.rules       /etc/udev/rules.d/90-book4-gpu-floor.rules
put 644 udev/61-book4-keyboard-not-a-tablet.rules /etc/udev/rules.d/61-book4-keyboard-not-a-tablet.rules
put 644 modules-load/book4-input.conf       /etc/modules-load.d/book4-input.conf
put 644 modules-load/i2c-dev.conf           /etc/modules-load.d/i2c-dev.conf

[ -z "$D" ] || { echo "DESTDIR set: files copied, nothing enabled"; exit 0; }
command -v earlyoom >/dev/null || echo "note: install the earlyoom package for the memory guard"
[ -f /usr/lib/ladspa/fast_lookahead_limiter_1913.so ] || echo "note: install swh-plugins; the speaker limiter needs it"
command -v i2ctransfer >/dev/null || echo "note: install i2c-tools; the charging daemon needs i2ctransfer/i2cget/i2cset"
systemctl daemon-reload
udevadm control --reload
sysctl -q --system
systemctl enable --now zram-swap.service book4-pd-charge.service
command -v earlyoom >/dev/null && systemctl enable --now earlyoom.service
echo "done. Log out and in (or restart pipewire/wireplumber) for the audio and camera config."
