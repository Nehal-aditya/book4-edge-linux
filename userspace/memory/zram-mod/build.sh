#!/bin/bash
# Build zsmalloc.ko + zram.ko out of tree and install them for the running kernel.
#
# Only for a kernel built without CONFIG_ZRAM (the community builds before this
# port's config). The modules are tied to the exact kernel version string: re-run
# after every kernel update.
#
#   usage: sudo ./build.sh <kernel source tree matching the running kernel>
set -euo pipefail
K=${1:?kernel source tree}
B=/lib/modules/$(uname -r)/build
W=$(cd "$(dirname "$0")" && pwd)
E=/lib/modules/$(uname -r)/extra
[ -f "$B/Module.symvers" ] || { echo "no kernel build dir at $B"; exit 1; }
[ -f "$K/mm/zsmalloc.c" ] || { echo "$K is not a kernel source tree"; exit 1; }
if grep -q '^CONFIG_ZRAM=' "$B/.config" 2>/dev/null; then
  echo "this kernel already has CONFIG_ZRAM; nothing to build"; exit 0
fi

rm -rf "$W/zsmalloc" "$W/zram"; mkdir -p "$W/zsmalloc" "$W/zram"
cp "$K/mm/zsmalloc.c" "$W/zsmalloc/"
for h in $(grep -oE '#include "[^"]+"' "$K/mm/zsmalloc.c" | sed 's/#include "//;s/"//'); do
  [ -f "$K/mm/$h" ] && cp "$K/mm/$h" "$W/zsmalloc/"
done
cat > "$W/zsmalloc/Kbuild" <<'K'
obj-m := zsmalloc.o
ccflags-y := -DCONFIG_ZSMALLOC=1 -DCONFIG_ZSMALLOC_CHAIN_SIZE=8
K
cp "$K"/drivers/block/zram/*.c "$K"/drivers/block/zram/*.h "$W/zram/"
cat > "$W/zram/Kbuild" <<'K'
zram-y := zcomp.o zram_drv.o backend_lz4.o backend_zstd.o
obj-m := zram.o
ccflags-y := -DCONFIG_ZRAM=1 -DCONFIG_ZRAM_BACKEND_LZ4=1 -DCONFIG_ZRAM_BACKEND_ZSTD=1 -DCONFIG_ZRAM_DEF_COMP=\"lz4\"
K
make -C "$B" M="$W/zsmalloc" -j"$(nproc)" modules
make -C "$B" M="$W/zram" KBUILD_EXTRA_SYMBOLS="$W/zsmalloc/Module.symvers" -j"$(nproc)" modules
install -D -m644 "$W/zsmalloc/zsmalloc.ko" "$E/zsmalloc.ko"
install -D -m644 "$W/zram/zram.ko" "$E/zram.ko"
depmod -a
echo "installed zram for $(uname -r): $(modinfo -F vermagic "$E/zram.ko")"
