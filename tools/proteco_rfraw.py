#!/usr/bin/env python3
"""Proteco MOVER .sub -> Sonoff RF Bridge RfRaw (Portisch 0xB0) payload.

  proteco_rfraw.py [-n FRAMES] file.sub...

Takes a Proteco key file (Protocol: Proteco), a RAW capture (Protocol: RAW) or a
BinRAW key file (Protocol: BinRAW). RAW and BinRAW are decoded with the same rules as
the firmware decoder (lib/subghz/protocols/proteco.c). Send the result from Tasmota as
`RfRaw <payload>; RfRaw 0`.

Frame: start HIGH, 24 bit pairs, then the guard LOW last. Portisch-style bridge
firmware (Portisch, mightymos/RF-Bridge-OB38S003) keeps the radio at the frame's last
level between repeats and only switches it off after the final one. With the guard last,
the time between repeats lengthens the guard, which tolerates it, not the last data bit,
which is rejected once it is ~150us too long. The repeat byte is FRAMES - 1: the
firmware sends repeat byte + 1 frames. The first frame follows silence, not a guard, so
FRAMES - 1 of them count. 10 frames is one real press, as the Flipper encoder sends.
"""
import argparse
import re
import sys

TE, LONG = 440, 880  # decoder timings, us
DELTA = 150
GUARD_US = 9050  # sent guard: measured on a Proteco TX3, Te 441us, guard 20.5 Te
BITS = 24


def near(d, nominal, delta):
    return abs(d - nominal) < delta


def decode(samples):
    """Keys of the complete frames in a signed duration list (+ HIGH, - LOW).

    The firmware's state machine: any LOW of 4 Te or more may open a frame, and once its
    24 bits are in, that guard must be 18.5..22.5 Te of the frame's own clock, measured
    over its 73 Te of pulses (start bit + 3 Te per bit).
    """
    keys, step, data, bits, last, te, guard = [], 0, 0, 0, 0, 0, 0
    for s in list(samples) + [-100000]:  # closing gap: report a frame that ends the input
        level, d = s > 0, abs(s)
        if step == 0:
            if not level and d >= TE * 4:
                guard, step = d, 1
        elif step == 1:
            if level and near(d, TE, DELTA):
                data, bits, te, step = 0, 0, d, 2
            else:
                step = 0
        elif step == 2:
            if level:
                step = 0
            elif d >= TE * 4:  # this gap ends the frame and is the next frame's guard
                frame_te = te // (1 + BITS * 3)
                if bits == BITS and frame_te * 37 // 2 <= guard <= frame_te * 45 // 2:
                    keys.append(data)
                guard, step = d, 1
            else:
                last, te, step = d, te + d, 3
        else:
            one = near(last, LONG, DELTA) and near(d, TE, DELTA)
            zero = near(last, TE, DELTA) and near(d, LONG, DELTA)
            if level and bits < BITS and (one or zero):
                data, bits, te, step = data << 1 | one, bits + 1, te + d, 2
            else:
                step = 0
    return keys


def samples_of(text):
    """Signed durations of a RAW or BinRAW file."""
    if re.search(r"^Protocol: BinRAW\s*$", text, re.M):
        te = int(re.search(r"^TE: (\d+)", text, re.M).group(1))
        bits = ""
        for count, data in re.findall(r"^Bit_RAW: (\d+)\s*\nData_RAW: ([0-9A-Fa-f ]+)$", text, re.M):
            count, raw = int(count), bytes.fromhex(data)
            if len(raw) != (count + 7) // 8:
                raise ValueError("Bit_RAW %d does not match %d Data_RAW bytes" % (count, len(raw)))
            bits += "".join(f"{b:08b}" for b in raw)[len(raw) * 8 - count :]  # right aligned
        out = []
        for bit in bits:
            d = te if bit == "1" else -te
            if out and (out[-1] > 0) == (d > 0):
                out[-1] += d
            else:
                out.append(d)
        return out
    return [int(x) for line in re.findall(r"^RAW_Data: (.*)$", text, re.M) for x in line.split()]


def keys_of(text):
    """(key, frames) of a .sub file, most frequent key first."""
    protocol = re.search(r"^Protocol: (\S+)", text, re.M)
    protocol = protocol.group(1) if protocol else ""
    if protocol == "Proteco":
        bit = re.search(r"^Bit: (\d+)", text, re.M)
        key = re.search(r"^Key: ([0-9A-Fa-f ]+)$", text, re.M)
        if not bit or int(bit.group(1)) != BITS or not key:
            raise ValueError("broken Proteco key file")
        return [(int(key.group(1).replace(" ", ""), 16) & 0xFFFFFF, 0)]
    if protocol not in ("RAW", "BinRAW"):
        raise ValueError(f"Protocol {protocol or '?'} is not Proteco, RAW or BinRAW")
    found = decode(samples_of(text))
    return sorted(((k, found.count(k)) for k in set(found)), key=lambda kf: -kf[1])


def payload(key, frames=10):
    nib = [0x8]  # start: HIGH Te (bucket 0, high bit = HIGH)
    for b in range(BITS - 1, -1, -1):
        nib += [1, 0x8] if key >> b & 1 else [0, 0x9]  # 1: LOW 2Te HIGH Te, 0: LOW Te HIGH 2Te
    nib.append(2)  # guard: LOW
    data = bytes(nib[i] << 4 | nib[i + 1] for i in range(0, len(nib), 2))
    buckets = b"".join(t.to_bytes(2, "big") for t in (TE, LONG, GUARD_US))
    body = bytes([3, frames - 1]) + buckets + data
    return ("AAB0" + bytes([len(body)]).hex() + body.hex() + "55").upper()


def bridge_sends(p, between_us):
    """What the bridge transmits: repeat byte + 1 frames, the last level held between them."""
    b = bytes.fromhex(p)
    buckets = [int.from_bytes(b[5 + 2 * i : 7 + 2 * i], "big") for i in range(b[3])]
    frame = [(n & 8, buckets[n & 7]) for x in b[5 + 2 * b[3] : -1] for n in (x >> 4, x & 0xF)]
    out = [-50000]  # radio off before
    for f in range(b[4] + 1):
        if f:
            out[-1] += between_us if out[-1] > 0 else -between_us
        for high, t in frame:
            d = t if high else -t
            if (out[-1] > 0) == (d > 0):
                out[-1] += d
            else:
                out.append(d)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("-n", "--frames", type=int, default=10, help="frames per send, 2..256 (default 10)")
    ap.add_argument("files", nargs="+", metavar="file.sub")
    args = ap.parse_args()
    if not 2 <= args.frames <= 256:
        ap.error("--frames must be 2..256")

    failed = False
    for path in args.files:
        try:
            with open(path) as f:
                found = keys_of(f.read())
        except (OSError, ValueError, AttributeError) as e:
            print(f"{path}: {e}", file=sys.stderr)
            failed = True
            continue
        if not found:
            print(f"{path}: no Proteco frame", file=sys.stderr)
            failed = True
            continue
        key, frames = found[0]
        p = payload(key, args.frames)
        # the payload must decode back to the key even with a slow bridge between repeats
        for between in (0, 800):
            got = decode(bridge_sends(p, between))
            assert got == [key] * (args.frames - 1), (between, got)
        seen = f", {frames} frame(s)" if frames else ""
        others = "".join(f", also 0x{k:06X} x{n}" for k, n in found[1:])
        print(f"{path}: key 0x{key:06X}{seen}{others}\nRfRaw {p}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
