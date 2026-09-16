# Debugging notes

Methods and traps from getting this camera working. Written down because several
cost hours and at least three led to confident but wrong conclusions.

## Measure only through libcamera

`v4l2-ctl --stream-mmap` on the CAMSS video node does not call `phy_configure`,
so the CSIPHY runs unpowered with its timer at XO (19.2 MHz instead of 400 MHz):

```
v4l2-ctl path : phytimer=19200000    rails use=0     <- PHY dead
cam (libcamera): phytimer=266666666  rails use=1     <- correctly powered
```

Every raw-v4l2 measurement is therefore worthless, including register sweeps that
look authoritative. Use `cam`.

## Read the metadata before tuning anything

```
cam -c1 --capture=120 --metadata
```

One command reported `ColourTemperature = 3011`, `ColourGains = [1.106, 3.019]`,
`ExposureTime = 33060`, `AnalogueGain = 15.5`: warm light, 3× blue gain, and
the sensor pinned at both its exposure and gain ceilings. That reframed the whole
colour problem and would have saved hours of matrix-nudging.

## Never assume a CSIPHY exists; read its hardware version

An absent PHY reads `CSIPHY 3PH HW Version = 0x00000000`; a present one reads
`0x40010000`. csiphy1 and csiphy2 are absent on this die. Measuring two and
generalising to the third was a mistake that nearly closed off the correct answer.

`journalctl --list-boots` is persistent here, so the version from every past boot
can be recovered for free rather than re-tested.

## Judge orientation with TEXT, never by eye

Watching a doorway move across the frame cannot distinguish a 180° rotation from a
horizontal mirror. Hold up a phone showing text. Doing this the lazy way produced a
wrong conclusion, a correct fix that was then reverted, and a second wrong
conclusion in the opposite direction.

## Frame completion comes from CSID, not VFE

`camss-vfe-680.c` has a stub ISR (`return IRQ_HANDLED;`) and `vfe_wm_start()` calls
`vfe_disable_irq()` with the comment *"We don't process IRQs for VFE in RDI mode at
the moment"*. Zero VFE interrupts is by design. Buffer completion arrives via
the CSID buf-done interrupt through `camss_buf_done()`, so CSID's interrupt count is
the frame counter. Several hours were spent treating intended behaviour as the bug.

## CSID IRQ status registers are *masked* status

Reading them while the mask is zero always returns zero, which reads as "no
activity" when it means "nothing enabled". Unmask first (`/dev/mem` write to the
mask register) or the reading is meaningless.

## /dev/mem works, with one rule

`CONFIG_STRICT_DEVMEM=y` but `IO_STRICT_DEVMEM` unset, so MMIO reads and writes work
from userspace (python `mmap` at a page-aligned base). Only touch CAMSS registers
while streaming: reading them unclocked hangs the bus.

Useful offsets, correct for the standalone PHY driver (mainline camss's 0x800/0xb0
are wrong for it): CSIPHY common block at `0x1000`, `CTRL_n = 0x1000 + 4n`,
`STATUS_n = 0x10b0 + 4n`, settle count at `+0x008` of each lane block
(`0x000/0x400/0x800/0xc00`, clock lane `0xe00`).

## The PHY driver never requests an IRQ

`phy-qcom-mipi-csi2` contains an ISR but no `request_irq`/`platform_get_irq`, so it
is dead code and no csiphy IRQ line is registered. PHY status must be polled.

## Sensor I²C bus numbers are not stable across boots

Seen as both `4-0036` and `8-0036`. Always resolve with
`readlink -f /sys/bus/i2c/devices/*-0036`.

## Verify what actually reached the sensor

Reading back all 220 registers the driver writes, while streaming, confirmed the
sensor was programmed exactly as intended and eliminated a whole class of theory.
CCI rejects long combined transfers, so read one byte at a time.

## Hot-swap modules instead of rebooting

```
modprobe -r qcom_camss; modprobe -r <mod>; install new .ko; depmod -a
modprobe <mod>; modprobe qcom_camss
```

camss releases the sensor and PHY, then rebinds. Note `phy-qcom-mipi-csi2` is in
the initramfs (so a reboot reverts it) while `ov02c10` is not (so the
`/lib/modules` copy is what boots).

## Tuning iteration needs no rebuild

Edit the YAML, install it, reopen the camera; it is re-read on each camera open.

## Two white references disagree

A painted wall reads warm; copier paper reads blue because of optical brighteners.
Measured on one frame they disagreed in opposite directions and bracketed neutral.
Don't chase a colour cast using either alone, and measure per luminance band:
a whole-frame average hid a cast that was plainly visible in the highlights.
