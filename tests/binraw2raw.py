#!/usr/bin/env python3
"""Convert Flipper BinRAW key files to RAW files:  binraw2raw.py OUT_DIR file.sub...

BinRAW keeps the signal as bits of TE us each, 1 = HIGH, right aligned in every
Bit_RAW/Data_RAW chunk. Chunks are joined (as lib/subghz/protocols/bin_raw.c does
when it sends them) and every run of equal bits becomes one RAW_Data duration.
A BinRAW chunk ends on the frame's last HIGH, so one guard of LOW is appended:
without it a decoder reading the RAW file never sees the frame end.
"""
import itertools
import os
import re
import sys

GUARD_TE = 21  # Proteco inter-frame gap, in Te
PER_LINE = 512  # samples per RAW_Data line, as the Flipper writes them


def convert(text):
    header = dict(re.findall(r"^(\w+): (.*)$", text, re.M))
    if header.get("Protocol") != "BinRAW":
        raise ValueError("not a BinRAW file")
    te = int(header["TE"])
    bits = ""
    for count, data in re.findall(r"^Bit_RAW: (\d+)\s*\nData_RAW: ([0-9A-Fa-f ]+)$", text, re.M):
        count, raw = int(count), bytes.fromhex(data)
        if len(raw) != (count + 7) // 8:
            raise ValueError("Bit_RAW %d does not match %d Data_RAW bytes" % (count, len(raw)))
        bits += "".join(f"{b:08b}" for b in raw)[len(raw) * 8 - count :]
    if not bits:
        raise ValueError("no Bit_RAW/Data_RAW data")

    samples = [(len(list(run)) * te) * (1 if level == "1" else -1)
               for level, run in itertools.groupby(bits)]
    if samples[-1] < 0:
        samples[-1] -= GUARD_TE * te
    else:
        samples.append(-GUARD_TE * te)

    out = ["Filetype: Flipper SubGhz RAW File", "Version: 1",
           "Frequency: " + header["Frequency"], "Preset: " + header["Preset"], "Protocol: RAW"]
    for i in range(0, len(samples), PER_LINE):
        out.append("RAW_Data: " + " ".join(map(str, samples[i : i + PER_LINE])))
    return "\n".join(out) + "\n"


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out_dir = sys.argv[1]
    os.makedirs(out_dir, exist_ok=True)
    for path in sys.argv[2:]:
        name = os.path.splitext(os.path.basename(path))[0] + "_raw.sub"
        with open(path) as f:
            text = f.read()
        try:
            raw = convert(text)
        except (ValueError, KeyError) as e:
            print(f"{path}: skipped, {e}", file=sys.stderr)
            continue
        with open(os.path.join(out_dir, name), "w") as f:
            f.write(raw)
        print(f"{path} -> {os.path.join(out_dir, name)}")


if __name__ == "__main__":
    main()
