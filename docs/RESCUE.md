# If it does not boot

Keep a second install on a USB stick (Arch ARM, this kernel + DTB) made while the
machine works. The internal ESP is a normal FAT partition; from the stick:

```sh
mount /dev/sda1 /mnt/esp          # ESP (systemd-boot lives at EFI/Microsoft/Boot/bootmgfw.efi)
mount /dev/sda2 /mnt/root         # root
```

- The boot menu is hidden if `loader.conf` has `timeout menu-hidden`; hold SPACE
  during power-on, or run `systemctl reboot --boot-loader-menu=15` from a session.
- Keep the previous kernel/initramfs/DTB as their own entry. Never overwrite the
  files an entry points at; add new files and a new entry.
- A black screen after the kernel starts is usually `msm` (the display driver)
  not loading or the panel regulator being turned off. `console=tty0` on the
  cmdline with a verbose `loglevel` shows which.
- The USB stick can win the firmware's boot order and make the internal install
  look broken. Unplug it before concluding hardware died.
- `efi=noruntime` is required; without it the kernel hangs early.
