#!/bin/sh
# Build and install the boot splash. Requires: libdrm, a systemd-boot entry with
# `splash` on its command line, and SDDM (the only tested display manager).
# Keep one boot entry WITHOUT `splash`: it runs no splash daemon at all.
set -eu
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
cd "$(dirname "$0")"
make
install -Dm755 book4-splash /usr/local/bin/book4-splash
install -Dm644 theme.conf /etc/book4-splash/theme.conf
for u in book4-splash book4-splash-release book4-splash-shutdown book4-splash-msm-load; do
	install -Dm644 units/$u.service /etc/systemd/system/$u.service
done
install -Dm644 units/book4-splash-msm-gate.conf /etc/modprobe.d/book4-splash-msm-gate.conf
install -Dm644 units/sddm-release.conf /etc/systemd/system/sddm.service.d/book4-splash-release.conf
install -Dm644 units/sddm-restart-tolerance.conf /etc/systemd/system/sddm.service.d/restart-tolerance.conf
# Two splash daemons must never contend for DRM master.
systemctl mask plymouth-start.service plymouth-read-write.service plymouth-quit.service \
	plymouth-quit-wait.service plymouth-poweroff.service plymouth-reboot.service \
	plymouth-halt.service plymouth-kexec.service >/dev/null 2>&1 || true
systemctl daemon-reload
systemctl enable book4-splash.service book4-splash-release.service \
	book4-splash-shutdown.service book4-splash-msm-load.service
echo "installed. Optional picture: ./mkground.sh <image> [logo.png]"
echo "The msm gate is now active: the display driver loads from the splash or the msm-load unit only."
systemd-analyze verify /etc/systemd/system/book4-splash.service /etc/systemd/system/book4-splash-release.service 2>&1 | head -5 || true
