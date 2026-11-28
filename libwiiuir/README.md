# libwiiuir

Send infrared remote codes (TV, AV receiver, set-top box...) from the Wii U
GamePad's IR LED, built on wut. The Wii U port of
[libwiiir](https://github.com/quatric/libwiiir), with the same calls.

Based on [WiiIR](https://github.com/dakotath/WiiIR) by Dakota Thorpe, used with
permission. See `NOTICE`.

- Loads [Flipper-IRDB](https://github.com/Lucaslhm/Flipper-IRDB) `.ir` files
  as they are: parsed codes and raw captures
- Every protocol Flipper-IRDB uses: NEC, NECext, NEC42, NEC42ext, Samsung32,
  RC5, RC5X, RC6, SIRC (12/15/20), Kaseikyo, RCA, Pioneer, plus JVC and raw
  timings
- Pronto hex (learned `0000` codes)

## How it sends

On the Wii U the console's sensor bar is powered by the SMC, not by a GPIO the
PowerPC can switch, so it cannot make a 38 kHz carrier the way the Wii does.
The GamePad has an IR LED (the one its TV remote uses), reachable through
`CCRCDCPerIrdaControl`. That port speaks IrDA SIR, which flashes the LED once
for every 0 bit; at a bit rate equal to the carrier (36000, 38400, 48000 or
57600) a run of 0 bits is the carrier and 1 bits are the gaps. `ir_sir.c` turns
the timings into those bytes.

**This transmitter has not been tried on hardware yet.** Carriers within 1/8 of
those rates work in principle (36-40 kHz and 56 kHz codes); others return -1.
The DRC wraps each send in a short packet header, which goes out ahead of the
code as a brief burst that receivers should ignore.

## Build

Requires devkitPro with wut.

```sh
export DEVKITPRO=/opt/devkitpro
make            # produces lib/libwiiuir.a
make install    # copies into $DEVKITPRO/portlibs/wiiu
make test       # host test of the encoders, .ir reader and SIR bytes
```

As a submodule:

```sh
git submodule add https://github.com/quatric/libwiiuir.git libwiiuir
```

then build it from your Makefile (`$(MAKE) -C libwiiuir`) and add
`-Ilibwiiuir/include` to your includes, `-Llibwiiuir/lib -lwiiuir` to your libs.

## Use

```c
#include <ir_wiiu.h>

ir_remote tv;
if (IR_LoadRemote("fs:/vol/external01/ir/Samsung_TV.ir", &tv) == 0) {
    IR_Send(IR_FindSignal(&tv, "Power"));   /* names as in the file */
    IR_FreeRemote(&tv);
}

IR_SendCode(IR_PROTO_NEC, 0x04, 0x08);      /* or a code directly */
```

With [libwiiuinput](https://github.com/quatric/libwiiuinput), map
`INPUT_ButtonsDown()` bits to signal names exactly as the libwiiir README shows.

`IR_Send*` blocks until the code has been sent and returns 0, or a negative
value. It uses one set of buffers: send from one thread.

`source/ir_encode.c`, `source/ir_file.c`, `source/ir_db.c`, `tools/irdb_xml.c` and `tests/ir_test.c` are the same
files as in libwiiir; change them in both.

## The device database (irdb.xml)

Flipper-IRDB (CC0) as XML in WiiIR's schema, so devices and buttons can be
added by hand:

```xml
<Manufacturers>
  <Manufacturer name="Samsung"><DeviceList>
    <DeviceEntry name="UE40" category="TVs">
      <ButtonEntry name="Power"><Data>NECext:0x7F00,0xEA15</Data></ButtonEntry>
      <ButtonEntry name="Vol_up"><Data frequency="38000" duty="0.33">9000 4500 560 ...</Data></ButtonEntry>
      <ButtonEntry name="Mute"><Data>RAW:0000 006C 0022 0002 ...</Data></ButtonEntry>
    </DeviceEntry>
  </DeviceList></Manufacturer>
</Manufacturers>
```

`<Data>` is `Protocol:address,command` (Flipper's values, decimal or `0x`
hex), raw timings in microseconds with a `frequency` (and `duty`), or
`RAW:` with Pronto hex. Address, command and raw timings are kept exactly as
Flipper-IRDB has them.

```sh
make irdb IRDB=path/to/Flipper-IRDB
```

This writes `irdb.xml` (Flipper-IRDB's own folders: 1,957 devices, 7.5 MB) and
`irdb_converted.xml` (its `_Converted_` folder: 6,944 more devices, 60 MB).

```c
ir_db db = {0};
IR_DbAdd(&db, "fs:/vol/external01/wiiu/apps/MyApp/irdb.xml");
IR_DbAdd(&db, "fs:/vol/external01/wiiu/apps/MyApp/irdb_custom.xml");  /* your own, kept across updates */

int tv = IR_DbFind(&db, "TVs", "Samsung", "Samsung_BN59-01175N");
ir_remote r;
if (tv >= 0 && IR_DbLoad(&db, tv, &r) == 0) { ... IR_FreeRemote(&r); }
IR_DbFree(&db);
```

`IR_DbAdd` only indexes a file (`db.devices[]`: manufacturer, category, name,
button count). `IR_DbLoad` reads one device when it is needed, so the
database never has to fit in memory.

## License

Apache License 2.0, see `LICENSE` and `NOTICE`.
