# Proteco MOVER for Flipper Zero

Read, decode, save and send Proteco MOVER gate remotes (Italian sliding gate motors) on Flipper
Zero without patching the firmware. A firmware patch that adds the same protocol to the stock
Sub-GHz app is included too.

Upstream issue: https://github.com/flipperdevices/flipperzero-firmware/issues/2345

## Protocol

433.92 MHz, AM650 (OOK), 24 bit fixed code. CAME line coding at a slower clock, which is why the
CAME decoder rejects it:

| part  | levels                         |
|-------|--------------------------------|
| guard | LOW ~21 Te (~9.1 ms)           |
| start | HIGH Te                        |
| bit 1 | LOW 2 Te, HIGH Te              |
| bit 0 | LOW Te, HIGH 2 Te              |

Te is ~440 us, bits go MSB first, and the frame repeats while the button is held.

## App (FAP)

- **Read**: listen on 433.92 MHz. Shows the key, the frame count and an RSSI bar. Press OK to use the key.
- **Open / Decode .sub**: browse `/ext/subghz`.
  - A `Protocol: Proteco` key file is loaded as is.
  - A RAW capture (`Protocol: RAW`) is decoded, and the first key found is shown with its frame count.
  - A BinRAW key file (`Protocol: BinRAW`, what the stock app saves when it reads an unknown signal
    with BinRAW on) is decoded the same way, from its `TE` and `Bit_RAW`/`Data_RAW` chunks.
- **Add Manually**: type the 24 bit key.
- **Key screen**: hold **OK** to send. Each burst is 10 frames, and bursts repeat while OK is held.
  Press **Right** to save the key as a native `.sub` file in `/ext/subghz`, which turns a RAW or
  BinRAW capture into a key file.

Saved files use the same format as the firmware patch:

```
Filetype: Flipper SubGhz Key File
Version: 1
Frequency: 433920000
Preset: FuriHalSubGhzPresetOok650Async
Protocol: Proteco
Bit: 24
Key: 00 00 00 00 00 12 34 56
```

A key saved by the app also works in the stock Sub-GHz app on patched firmware, and the other way round.
On unpatched firmware, the stock app doesn't know the `Proteco` protocol, so open those files with this app.

An external CC1101 module on the GPIO header is used when one is attached, otherwise the internal radio.
The screen shows `Ext radio` or `Int radio`.
- A module plugged in while the app runs is picked up within about 5 s, at the next Read or OK press.
- A module pulled out falls back to the internal radio, also during Read.
- The app powers the module through the 5 V pin (OTG), and only switches that power off again if it
  turned it on itself.
- The amp/LED setting for E07-style modules comes from the firmware's Sub-GHz settings.

TX follows the firmware's region settings.

### Build

Inside a firmware checkout, for example Unleashed:

```sh
ln -s /path/to/flipper-proteco applications_user/proteco
./fbt fap_proteco                 # -> build/f7-firmware-*/.extapps/proteco.fap
./fbt launch APPSRC=proteco       # build, copy over USB and start
```

Or with [ufbt](https://github.com/flipperdevices/flipperzero-ufbt), using the SDK of the firmware on
your Flipper:

```sh
cd flipper-proteco && ufbt        # ufbt launch to install and start
```

Copy `proteco.fap` to `/ext/apps/Sub-GHz/` on the SD card.

### Host test

The app's codec (`proteco_codec.c`) is plain C. The test builds it together with the firmware's
`lib/subghz/protocols/proteco.c` from the patched checkout, and feeds every signal to both decoders,
which have to agree. It runs encode/decode round trips with timing jitter, then decodes the RAW and
BinRAW `.sub` files (all of `dumps/` by default). Each key it finds is re-encoded by the firmware
encoder and decoded again. For BinRAW files, the re-encoded frame has to match the capture bit for bit.

```sh
make -C tests                                   # FW=path/to/firmware if not ../unleashed-firmware
make -C tests RAW="my_capture.sub other.sub"
python3 tests/binraw2raw.py OUT_DIR dumps/*.sub  # BinRAW key files -> RAW files
```

## Sonoff RF Bridge (Tasmota)

`tools/proteco_rfraw.py` makes an `RfRaw` payload for a Sonoff RF Bridge running Portisch firmware,
or [mightymos/RF-Bridge-OB38S003](https://github.com/mightymos/RF-Bridge-OB38S003). It reads a
Proteco key file, a RAW capture or a BinRAW file. RAW and BinRAW files are decoded with the same
rules as the firmware decoder.

```sh
python3 tools/proteco_rfraw.py my_remote.sub           # 10 frames, one real press
python3 tools/proteco_rfraw.py -n 12 my_remote.sub     # more frames if the gate still misses some
```

```
my_remote.sub: key 0xDF2396, 186 frame(s)
RfRaw AAB021030901B80370235A8181809181818181809091809090918181809091809181809255
```

Send it from Tasmota as `RfRaw <payload>; RfRaw 0`. The payload puts the guard at the **end** of
each frame. These bridge firmwares keep the radio at the frame's last level between repeats. A
payload that ends on a HIGH has its last bit stretched by that delay, and the gate drops the frame
once the bit is ~150 µs too long. With the guard last, the delay only lengthens the guard. The
firmware sends repeat byte + 1 frames, and the first one follows silence rather than a guard, so 9
of the default 10 frames count. Every payload is decoded back before it's printed, both with no
delay between repeats and with 800 µs.

## Firmware patch

`proteco-mover-subghz.patch` adds a native `Proteco` protocol to Unleashed. It provides decode, save and
send from the stock Sub-GHz app, plus unit tests. Apply it from the firmware root:

```sh
git apply --3way /path/to/flipper-proteco/proteco-mover-subghz.patch
```

It needs Unleashed dev from 2026-08-31 (PR #1116) or later, which has the shared helpers in
`lib/subghz/protocols/common.h`. If `--3way` leaves a conflict, it will be in the one-line registry
entries in `protocol_items.c/.h`. Keep upstream's lines and append `&subghz_protocol_proteco,` /
`#include "proteco.h"` as the last entries. Placed higher up, they make clang-format (`./fbt lint`)
want the whole registry reflowed.
