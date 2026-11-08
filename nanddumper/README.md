# nanddumper@IOS stages

The IOS-side code GodModeWii's NAND backup (`source/nanddump.c`) runs, from
[nanddumper@IOS](https://github.com/Naim2000/nanddumper_ios) by thepikachugamer (GPL-2.0, see `LICENSE.md`),
commit `e2b5937faf4119bb5f66979b6c104f6f086b9c75`, unchanged.

* `stage0` — the thumb trampoline the `/dev/sha` exploit (found by mkwcat) jumps to; its words are inlined in
  `source/nanddump.c`.
* `stage1` — starts an IOS thread with the given entry point (`res/nd_stage1.bin`).
* `realcode` — that thread: turns off the PPC's memory protections, becomes root and serves `/dev/realcode`
  (virtual to physical addresses, cache maintenance) so the FS module can be patched to open `/dev/flash`
  (`res/nd_realcode.bin`).

The `.bin` files in `res/` are the ones nanddumper@IOS ships prebuilt. Building them again needs the
StarStruck SDK (`SDKDIR`); run `make` in each folder and copy `stage1.bin` / `realcode.bin` to
`res/nd_stage1.bin` / `res/nd_realcode.bin`.
