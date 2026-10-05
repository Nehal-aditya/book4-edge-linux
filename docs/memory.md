# Memory: the freezes and what fixes them

Under load the machine used to freeze for minutes while `free` still showed
gigabytes available and most of the zram swap was unused. Two kernel-side
problems caused it. Both are handled in this repo: a patch for the first, a
config option plus one tmpfiles line for the second.

## A free-CMA counter that drifts

The device tree reserves one CMA pool of 128 MB (`linux,cma`, 32768 pages).
During uptime the kernel's count of free CMA pages, `NR_FREE_CMA_PAGES`, grows
far past that pool, and `/proc/meminfo` then shows `CmaFree` above `CmaTotal`.
One capture read 34013 pages against 11642 really free on the CMA free lists,
and the DMA zone counted 111 free CMA pages while holding no CMA pageblocks at
all. The pool itself is fine; only the counter is wrong. It moves in jumps of
5000 to 12000 pages, and what starts it is not known yet.

The page allocator subtracts that counter from the free memory seen by every
allocation that cannot use CMA (`__zone_watermark_unusable_free()`). With the
drifted value usable memory goes negative, so kswapd, zram and atomic
allocations (the Wi-Fi driver's receive buffers among them) fail and the
desktop stalls.

`patches/0004` caps the value at the zone's real CMA size, in that check and in
the allocator's CMA balance. With a correct counter it changes nothing.

To see whether your kernel drifts:

```sh
grep -E 'CmaTotal|CmaFree' /proc/meminfo           # CmaFree above CmaTotal is the drift
cat /sys/kernel/mm/cma/linux,cma/available_pages     # the pool's own count (CONFIG_CMA_SYSFS)
```

## Reclaim that thrashes

Without MGLRU the kernel's reclaim drops program code under pressure and reads
it back from disk again and again, so the machine looks frozen for minutes
before the OOM killer acts. `config/book4-edge.config` now builds MGLRU
(`CONFIG_LRU_GEN`), and `userspace/memory/mglru.conf` sets `min_ttl_ms=1000`,
the value the kernel documentation gives against thrashing: a squeeze then ends
with one process stopped instead of a frozen desktop. On the X1P42100
`lru_gen/enabled` reads `0x0001`, which is all this CPU offers.

## Status

First booted on 2026-09-28 with the patch, MGLRU and in-tree zram. After that
boot `CmaFree` stayed inside the pool, and Wi-Fi, Bluetooth, audio, camera and
the EC driver all worked. The drift used to take hours to days of
uptime to show, so a clean result over several days is still to come.

Out-of-tree modules have to be rebuilt for every new kernel release:
`driver/samsung-galaxybook-ec` and, on a DTB without the Type-C buses,
`userspace/charging/i2c-wake`.
