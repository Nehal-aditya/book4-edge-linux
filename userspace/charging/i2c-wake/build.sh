#!/bin/bash
# Compile overlay.dts into the module and install it for the running kernel.
set -euo pipefail
W=$(cd "$(dirname "$0")" && pwd)
B=/lib/modules/$(uname -r)/build
[ -f "$B/Module.symvers" ] || { echo "no kernel build dir at $B"; exit 1; }
DTC=$B/scripts/dtc/dtc; [ -x "$DTC" ] || DTC=dtc
"$DTC" -@ -I dts -O dtb -o "$W/overlay.dtbo" "$W/overlay.dts"
python3 - "$W/overlay.dtbo" > "$W/overlay_blob.h" <<'PY'
import sys
b = open(sys.argv[1], 'rb').read()
print('static const unsigned char overlay_dtbo[] = {')
for i in range(0, len(b), 12):
    print('\t' + ', '.join('0x%02x' % x for x in b[i:i+12]) + ',')
print('};\nstatic const unsigned int overlay_dtbo_len = %d;' % len(b))
PY
make -C "$B" M="$W" modules
install -D -m644 "$W/i2c-wake.ko" "/lib/modules/$(uname -r)/extra/i2c-wake.ko"
depmod -a
echo "installed i2c-wake for $(uname -r); the charging daemon loads it when the buses are absent"
