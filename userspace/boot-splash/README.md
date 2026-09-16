# Boot splash

Plymouth does not work on this hardware: its udev path drops the display driver's
card event, it opens the console and destroys the firmware logo, and it leaves a
gap of about two seconds of black. `book4-splash` is a small DRM client that
polls `/dev/dri`, takes master on the real card (never the firmware framebuffer),
composes the frame before the modeset, and hands the panel to the display manager
without a blank frame. It also owns loading `msm`, with the backlight off across
the driver's panel reset, which otherwise shows as a torn bright line.

Sequence: firmware logo, about 80 ms dark while msm resets the display block,
splash, then the compositor. With `theme.conf` matching the compositor's
background there is no visible cut.

- `install.sh` builds, installs the units and the msm gate, masks plymouth, and
  enables everything. SDDM is the only tested display manager.
- `mkground.sh <image> [logo.png]` makes an optional full-panel picture; without it
  the splash is the flat colour from `/etc/book4-splash/theme.conf`.
- `book4-splash --dry-run out.ppm` composes the frame to a file without touching
  the display.
- Keep one boot entry without `splash` on its command line. The daemon does not
  run there, and `book4-splash-msm-load.service` loads the driver instead.

The msm gate (`blacklist msm`) is required by the splash. Without the splash
installed, do not install the gate.
