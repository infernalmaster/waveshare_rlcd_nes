#!/usr/bin/env python3
"""Turn a .nes file into nes_rom_data.h, so the build carries the game and no
card is needed.

    python3 tools/embed_rom.py SomeGame.nes

Writes nes_rom_data.h next to the sketch. Set NES_EMBEDDED_ROM to 1 in
hw_config.h to actually use it.

A 256 KB ROM becomes about 1.4 MB of C source and adds its own size to the
binary, so check that your partition scheme still has room for the app.
"""
import os
import sys

BYTES_PER_LINE = 16

HEADER = """/* nes_rom_data.h - {src}, {size} bytes, as a flash array.
 *
 * GENERATED - do not edit by hand. Regenerate with tools/embed_rom.py to
 * embed a different game.
 *
 * Included by exactly ONE translation unit (nes_rom_embedded.cpp); it defines
 * the array rather than declaring it, so a second include would be a duplicate
 * symbol at link time.
 *
 * const, so it lands in .rodata and stays in flash instead of costing 256 KB of
 * RAM. The emulator never writes to it: rom_load() copies PRG and CHR into its
 * own PSRAM buffers and plays from those.
 */
#ifndef NES_ROM_DATA_H
#define NES_ROM_DATA_H

#include <stdint.h>

#define NES_ROM_NAME "{name}"

const uint8_t nes_rom_data[] = {{
"""

# Not passed through str.format, so braces here are literal. Doubling one the
# way HEADER has to is exactly the bug this comment exists to prevent.
FOOTER = """};

const unsigned nes_rom_size = sizeof nes_rom_data;

#endif /* NES_ROM_DATA_H */
"""


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2

    src = argv[1]
    data = open(src, "rb").read()

    if data[:4] != b"NES\x1a":
        print(f"{src}: not an iNES file - the first four bytes should be "
              f"'NES\\x1a', they are {data[:4]!r}", file=sys.stderr)
        return 1

    prg, chr_ = data[4], data[5]
    mapper = (data[6] >> 4) | (data[7] & 0xF0)
    print(f"{os.path.basename(src)}: mapper {mapper}, "
          f"{prg * 16} KB PRG, {chr_ * 8} KB CHR, {len(data)} bytes total")

    name = os.path.splitext(os.path.basename(src))[0]
    out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            os.pardir, "nes_rom_data.h")

    with open(out_path, "w") as out:
        out.write(HEADER.format(src=os.path.basename(src), size=len(data),
                                name=name))
        for i in range(0, len(data), BYTES_PER_LINE):
            row = data[i:i + BYTES_PER_LINE]
            out.write("    " + "".join("0x%02x," % b for b in row) + "\n")
        out.write(FOOTER)

    print(f"wrote {os.path.normpath(out_path)} "
          f"({os.path.getsize(out_path) / 1e6:.2f} MB of source)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
