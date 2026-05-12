#!/usr/bin/env python3
"""Decompress a Dolphin/standard CISO file to raw GC/Wii ISO.

CISO header layout:
    0x00: char[4]   magic = "CISO"
    0x04: uint32_le block_size (typical: 0x00200000 = 2 MiB)
    0x08: uint8[]   block_map (1 = block present, 0 = zero-fill)

The map continues to fill a fixed 32 KiB header. Present blocks are stored
sequentially after the header.

Usage:  ciso_to_iso.py <input.ciso> <output.iso>
"""

import argparse
import os
import struct
import sys

HEADER_SIZE = 0x8000   # 32 KiB total header
MAGIC = b"CISO"


def decompress(in_path: str, out_path: str) -> None:
    with open(in_path, "rb") as fin:
        header = fin.read(HEADER_SIZE)
        if len(header) < 8 or header[0:4] != MAGIC:
            sys.exit(f"error: {in_path} is not a CISO file (bad magic)")

        block_size = struct.unpack("<I", header[4:8])[0]
        block_map = header[8:HEADER_SIZE]

        # Determine the highest present block index.
        last_present = max(
            (i for i, b in enumerate(block_map) if b), default=-1)
        if last_present < 0:
            sys.exit("error: CISO has no present blocks")
        total_blocks = last_present + 1
        out_size = total_blocks * block_size

        print(f"  block_size:   {block_size:,} bytes")
        print(f"  total_blocks: {total_blocks:,}")
        print(f"  out_size:     {out_size:,} bytes ({out_size / 1024**3:.2f} GiB)")

        with open(out_path, "wb") as fout:
            zero_block = b"\x00" * block_size
            written = 0
            present = 0
            for i in range(total_blocks):
                if block_map[i]:
                    chunk = fin.read(block_size)
                    if len(chunk) != block_size:
                        sys.exit(
                            f"error: truncated CISO at block {i}: "
                            f"read {len(chunk)} of {block_size}")
                    fout.write(chunk)
                    present += 1
                else:
                    fout.write(zero_block)
                written += block_size
                if i % 64 == 0:
                    pct = 100.0 * written / out_size
                    print(f"  ... {pct:5.1f}% ({i+1}/{total_blocks} blocks, {present} present)",
                          end="\r", flush=True)
            print(f"\n  done: {present} present blocks, "
                  f"{total_blocks - present} zero-fill blocks")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("input", help="path to .ciso input")
    ap.add_argument("output", help="path to .iso output")
    args = ap.parse_args()

    if not os.path.exists(args.input):
        sys.exit(f"error: {args.input} does not exist")
    decompress(args.input, args.output)


if __name__ == "__main__":
    main()
