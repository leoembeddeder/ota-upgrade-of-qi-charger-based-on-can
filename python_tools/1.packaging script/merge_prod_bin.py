# -*- coding: utf-8 -*-
"""Merge bootloader.bin + app_slot_a.ota.bin into a single production bin.

The merged image is laid out to match the AT32F426 Flash layout:
  0x08000000  Bootloader  (28KB = 0x7000)
  0x08007000  Slot A      (app_slot_a.ota.bin, includes 256B XATO header)

Usage:
    python merge_prod_bin.py                          # use defaults
    python merge_prod_bin.py --boot <path> --app <path> --out <path>
    python merge_prod_bin.py --hex                    # also produce Intel HEX
"""

from __future__ import print_function

import argparse
import os
import struct
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APP_BIN_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "app bin")
BURN_BIN_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "burn bin")

DEFAULT_BOOT = os.path.join(
    REPO_ROOT,
    "qi_wireless_bootloader", "mdk_project", "Objects", "bootloader.bin",
)
DEFAULT_APP = os.path.join(APP_BIN_DIR, "app_slot_a.bin")
DEFAULT_OUT = os.path.join(BURN_BIN_DIR, "prod_image.bin")

BOOT_BASE = 0x08000000
BOOT_SIZE = 0x7000  # 28 KB reserved for bootloader
SLOT_A_SIZE = 0xA800  # 42 KB
SLOT_A_OFFSET = BOOT_SIZE  # Slot A starts at 0x7000 within merged image
APP_BASE = BOOT_BASE + SLOT_A_OFFSET  # 0x08007000


def bin_to_ihex(data, base_addr):
    """Convert raw bytes to Intel HEX; emit ELA when upper 16 bits change."""
    lines = []
    last_upper = None
    for i in range(0, len(data), 16):
        addr = base_addr + i
        upper = (addr >> 16) & 0xFFFF
        if upper != last_upper:
            ext = "02000004{:04X}".format(upper)
            check = (~sum(byte.fromhex(ext[j : j + 2]) for j in range(0, len(ext), 2)) + 1) & 0xFF
            lines.append(":{}{:02X}".format(ext, check))
            last_upper = upper
        chunk = data[i : i + 16]
        byte_count = len(chunk)
        address = addr & 0xFFFF
        record_type = 0x00
        line = "{:02X}{:04X}{:02X}".format(byte_count, address, record_type)
        line += chunk.hex().upper()
        check = (~sum(byte.fromhex(line[j : j + 2]) for j in range(0, len(line), 2)) + 1) & 0xFF
        line += "{:02X}".format(check)
        lines.append(":{}".format(line))
    lines.append(":00000001FF")
    return "\r\n".join(lines) + "\r\n"


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Merge Bootloader + Slot A into one production bin"
    )
    parser.add_argument(
        "--boot", default=DEFAULT_BOOT,
        help="bootloader.bin path (default: qi_wireless_bootloader/.../bootloader.bin)",
    )
    parser.add_argument(
        "--app", default=DEFAULT_APP,
        help="app_slot_a.ota.bin path (default: qi_wireless_code/.../app_slot_a.ota.bin)",
    )
    parser.add_argument(
        "--out", default=DEFAULT_OUT,
        help="Output merged bin path (default: python_tools/prod_image.bin)",
    )
    parser.add_argument(
        "--hex", action="store_true",
        help="Also produce an Intel HEX file alongside the bin",
    )
    args = parser.parse_args(argv)

    # ---- validate inputs ----
    if not os.path.isfile(args.boot):
        sys.stderr.write("ERROR: Bootloader not found: {}\n".format(args.boot))
        return 1
    if not os.path.isfile(args.app):
        sys.stderr.write("ERROR: APP image not found: {}\n".format(args.app))
        return 1

    boot_data = open(args.boot, "rb").read()
    app_data = open(args.app, "rb").read()

    # ---- size checks ----
    if len(boot_data) > BOOT_SIZE:
        sys.stderr.write(
            "ERROR: Bootloader too large: {} bytes (max {})\n".format(
                len(boot_data), BOOT_SIZE
            )
        )
        return 1
    if len(app_data) > SLOT_A_SIZE:
        sys.stderr.write(
            "ERROR: Slot A image too large: {} bytes (max {})\n".format(
                len(app_data), SLOT_A_SIZE
            )
        )
        return 1

    # Verify XATO magic in APP image
    if len(app_data) < 4 or app_data[:4] != b"XATO":
        sys.stderr.write(
            "WARNING: APP image does not start with XATO magic; "
            "are you sure this is a packed .ota.bin?\n"
        )

    # ---- merge ----
    padded_boot = boot_data.ljust(BOOT_SIZE, b"\xFF")
    merged = padded_boot + app_data

    # ---- write output ----
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(merged)

    print("Bootloader : {} ({} bytes)".format(args.boot, len(boot_data)))
    print("APP Slot A : {} ({} bytes)".format(args.app, len(app_data)))
    print("Merged     : {} ({} bytes)".format(args.out, len(merged)))
    print("Layout:")
    print("  0x{:08X}  Bootloader  {} bytes".format(BOOT_BASE, len(boot_data)))
    print("  0x{:08X}  Slot A      {} bytes (header + firmware)".format(APP_BASE, len(app_data)))
    print("  0x{:08X}  End".format(BOOT_BASE + len(merged)))
    print("")
    print("Flash command (J-Link):")
    print('  JLink> loadbin "{}", 0x{:08X}'.format(args.out, BOOT_BASE))
    print("")
    print("Flash command (AT32 ISP Tool):")
    print("  Open AT32 ISP Tool -> Select file -> {}".format(args.out))
    print("  -> Set start address 0x{:08X} -> Download".format(BOOT_BASE))

    # ---- optional HEX ----
    if args.hex:
        hex_path = os.path.splitext(args.out)[0] + ".hex"
        ihex = bin_to_ihex(merged, BOOT_BASE)
        with open(hex_path, "w") as f:
            f.write(ihex)
        print("")
        print("Intel HEX  : {} ({} bytes)".format(hex_path, len(ihex)))

    return 0


if __name__ == "__main__":
    sys.exit(main())
