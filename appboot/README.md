# appboot

The small loader GodModeWii uses to start `.dol` / `.elf` files from the file
browser. It is taken from [YAWM ModMii Edition](https://github.com/modmii/YAWM-ModMii-Edition)
(`source/boot`, GPL-2.0; OverjoY, blackb0x, scooby74029, Peter0x44, XFlak),
with `crt0.s` / `link.ld` by Segher Boessenkool. The only change is a local
replacement for `__exception_closeall`, which current libogc no longer exports.

It is built by the main Makefile into `data/appboot.bin` and linked in.
