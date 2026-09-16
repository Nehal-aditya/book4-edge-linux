#!/bin/sh
# Make the splash picture: an image filled to the panel, optionally with a logo
# centred on it, as raw BGRA with an 8-byte (u32 w, u32 h, little-endian) header so
# the daemon needs no image decoder on the boot path.
#
#   mkground.sh <image> [logo.png] [-o /usr/share/book4-splash/ground.bgra]
#
# Without a ground file the daemon paints the flat colour from /etc/book4-splash/theme.conf.
set -eu
W=1920; H=1080
OUT=/usr/share/book4-splash/ground.bgra
IMG=${1:?image}; shift
LOGO=""
while [ $# -gt 0 ]; do case "$1" in -o) OUT=$2; shift 2;; *) LOGO=$1; shift;; esac; done
header() {
	printf '%b' "$(printf '\\x%02x\\x%02x\\x%02x\\x%02x\\x%02x\\x%02x\\x%02x\\x%02x' \
		$(($1 & 255)) $(($1 >> 8 & 255)) $(($1 >> 16 & 255)) $(($1 >> 24 & 255)) \
		$(($2 & 255)) $(($2 >> 8 & 255)) $(($2 >> 16 & 255)) $(($2 >> 24 & 255)))"
}
tmp=$(mktemp); png=$(mktemp --suffix=.png)
magick "$IMG" -resize ${W}x${H}^ -gravity center -extent ${W}x${H} -alpha off "$png"
[ -z "$LOGO" ] || magick "$png" "$LOGO" -gravity center -composite -alpha off "$png"
header $W $H > "$tmp"
magick "$png" -depth 8 -alpha on BGRA:- >> "$tmp"
want=$((W * H * 4 + 8)); got=$(wc -c < "$tmp")
[ "$got" -eq "$want" ] || { echo "expected $want bytes, got $got" >&2; exit 1; }
install -D -m644 "$tmp" "$OUT"; rm -f "$tmp" "$png"
echo "$OUT ${W}x${H}"
