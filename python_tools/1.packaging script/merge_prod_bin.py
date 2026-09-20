# -*- coding: utf-8 -*-
"""Merge bootloader.bin + app_image.bin into a single production bin.

Single-App Flash layout (OTA-ARCH-0920):
  0x08000000  Bootloader (16KB = 0x4000)
  0x08004000  App image (XATO header + firmware, max 48KB = 0xC000)
  0x08010000  Backup region (OTA staging; left erased 0xFF in factory bin)
  0x0801C000  Metadata (left erased; BOOT self-heals defaults on first
              boot: meta_validate fails -> app_valid=0 -> image verify
              still runs directly on the App region -> flag set + jump)

Usage:
    python merge_prod_bin.py
    python merge_prod_bin.py --boot <path> --app <path> --out <path>
    python merge_prod_bin.py --hex
    python merge_prod_bin.py --force   # merge even without XATO magic
"""

from __future__ import print_function

import argparse
import os
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APP_BIN_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "app bin")
BURN_BIN_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "burn bin")

DEFAULT_BOOT = os.path.join(
    REPO_ROOT,
    "qi_wireless_bootloader", "mdk_project", "Objects", "bootloader.bin",
)
DEFAULT_APP = os.path.join(APP_BIN_DIR, "app_image.bin")
DEFAULT_OUT = os.path.join(BURN_BIN_DIR, "prod_image.bin")

BOOT_BASE = 0x08000000
BOOT_SIZE = 0x4000
APP_SIZE = 0xC000
APP_OFFSET = BOOT_SIZE          # App image starts at +0x4000 in merged bin
APP_BASE = BOOT_BASE + APP_OFFSET  # 0x08004000


def bin_to_ihex(data, base_addr):
    """Convert raw bytes to Intel HEX; emit ELA when upper 16 bits change."""
    lines = []
    last_upper = None
    for i in range(0, len(data), 16):
        addr = base_addr + i
        upper = (addr >> 16) & 0xFFFF
        if upper != last_upper:
            ext = "02000004{:04X}".format(upper)
            check = (~sum(bytes.fromhex(ext[j : j + 2]) for j in range(0, len(ext), 2)) + 1) & 0xFF
            lines.append(":{}{:02X}".format(ext, check))
            last_upper = upper
        chunk = data[i : i + 16]
        byte_count = len(chunk)
        address = addr & 0xFFFF
        record_type = 0x00
        line = "{:02X}{:04X}{:02X}".format(byte_count, address, record_type)
        line += chunk.hex().upper()
        check = (~sum(bytes.fromhex(line[j : j + 2]) for j in range(0, len(line), 2)) + 1) & 0xFF
        line += "{:02X}".format(check)
        lines.append(":{}".format(line))
    lines.append(":00000001FF")
    return "\r\n".join(lines) + "\r\n"


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Merge Bootloader + App image into one production bin")
    parser.add_argument("--boot", default=DEFAULT_BOOT,
                        help="bootloader.bin path (default: Objects/bootloader.bin)")
    parser.add_argument("--app", default=DEFAULT_APP,
                        help="packed App XATO image path (default: app bin/app_image.bin)")
    parser.add_argument("--out", default=DEFAULT_OUT,
                        help="output merged bin (default: burn bin/prod_image.bin)")
    parser.add_argument("--hex", action="store_true",
                        help="also produce Intel HEX next to the bin")
    parser.add_argument("--force", action="store_true",
                        help="merge even if the APP image does not start with "
                             "XATO magic (default: hard-fail)")
    args = parser.parse_args(argv)

    if not os.path.isfile(args.boot):
        sys.stderr.write("ERROR: Bootloader not found: {}\n".format(args.boot))
        return 1
    if not os.path.isfile(args.app):
        sys.stderr.write("ERROR: APP image not found: {}\n".format(args.app))
        return 1

    boot_data = open(args.boot, "rb").read()
    app_data = open(args.app, "rb").read()

    if len(boot_data) > BOOT_SIZE:
        sys.stderr.write("ERROR: Bootloader too large: {} bytes (max {})\n".format(
            len(boot_data), BOOT_SIZE))
        return 1
    if len(app_data) > APP_SIZE:
        sys.stderr.write("ERROR: App image too large: {} bytes (max {})\n".format(
            len(app_data), APP_SIZE))
        return 1
    if len(app_data) < 4 or app_data[:4] != b"XATO":
        # D4-R2: XATO magic mismatch is a hard gate by default — merging a
        # non-XATO payload into a production bin would brick the App slot.
        # --force downgrades to WARNING for deliberate overrides.
        if args.force:
            sys.stderr.write(
                "WARNING: APP image does not start with XATO magic; "
                "continuing because --force was given. "
                "Is this a packed app_image.bin?\n")
        else:
            sys.stderr.write(
                "ERROR: APP image does not start with XATO magic; "
                "is this a packed app_image.bin? "
                "Use --force to merge anyway.\n")
            sys.exit(1)

    padded_boot = boot_data.ljust(BOOT_SIZE, b"\xFF")
    merged = padded_boot + app_data

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(merged)

    print("Bootloader : {} ({} bytes)".format(args.boot, len(boot_data)))
    print("APP image  : {} ({} bytes)".format(args.app, len(app_data)))
    print("Merged     : {} ({} bytes)".format(args.out, len(merged)))
    print("Layout (single-App arch):")
    print("  0x{:08X}  Bootloader  {} bytes".format(BOOT_BASE, len(boot_data)))
    print("  0x{:08X}  App image   {} bytes (header + firmware)".format(APP_BASE, len(app_data)))
    print("  0x{:08X}  Backup      left erased (OTA staging)".format(0x08010000))
    print("  0x{:08X}  Metadata    left erased (BOOT defaults on 1st boot)".format(0x0801C000))
    print("  0x{:08X}  End".format(BOOT_BASE + len(merged)))
    print("")
    print("Flash command (J-Link):")
    print('  JLink> loadbin "{}", 0x{:08X}'.format(args.out, BOOT_BASE))
    print("")
    print("Flash command (AT32 ISP Tool):")
    print("  Select file -> {} -> start address 0x{:08X} -> Download".format(
        args.out, BOOT_BASE))

    if args.hex:
        hex_path = os.path.splitext(args.out)[0] + ".hex"
        ihex = bin_to_ihex(merged, BOOT_BASE)
        with open(hex_path, "w") as f:
            f.write(ihex)
        print("")
        print("Intel HEX  : {} ({} chars)".format(hex_path, len(ihex)))

    return 0


if __name__ == "__main__":
    sys.exit(main())
