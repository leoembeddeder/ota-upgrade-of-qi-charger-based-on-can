# -*- coding: utf-8 -*-
"""Pack the App Keil raw .bin into a XATO image (256B header + firmware).

Single-App architecture (OTA-ARCH-0920): one image, linked for the App
window (Keil IROM1 = 0x08004100). Factory flashing writes the packed
image at 0x08004000 (merge_prod_bin.py); OTA streams it into the Backup
region (0x08010000) and BOOT copies it into the App region after verify.

Header layout is byte-frozen: @0x4C is a 16B reserved placeholder (the
original version field was removed; packing fills 0x00). Version identity
lives only in firmware SW_VERSION_STR (can_protocol.c).
"""

from __future__ import print_function

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PARENT = os.path.dirname(HERE)
if PARENT not in sys.path:
    sys.path.insert(0, PARENT)
if HERE not in sys.path:
    sys.path.insert(0, HERE)

from zcanpro_ext_ota_auto import (  # noqa: E402
    APP_BASE,
    IMAGE_HEADER_SIZE,
    load_ec_private_key,
    pack_image_if_needed,
    validate_image,
)

REPO_ROOT = os.path.dirname(os.path.dirname(HERE))
APP_BIN_DIR = os.path.join(PARENT, "app bin")
DEFAULT_BIN = os.path.join(REPO_ROOT, "qi_wireless_code_app", "mdk_project",
                           "Objects", "qi_wireless_code_app.bin")
DEFAULT_KEY = os.path.join(REPO_ROOT, "docs", "keys", "private.pem")
DEFAULT_OUT_NAME = "app_image.bin"


def pack_one(bin_path, priv, out_name=DEFAULT_OUT_NAME):
    """Pack one App firmware; return 0 on success, 1 on failure."""
    if not os.path.isfile(bin_path):
        print("skip (not found): %s" % bin_path)
        return 1
    image = pack_image_if_needed(bin_path, priv)
    validate_image(image)

    if not os.path.isdir(APP_BIN_DIR):
        os.makedirs(APP_BIN_DIR)
    out_path = os.path.join(APP_BIN_DIR, out_name)
    with open(out_path, "wb") as f:
        f.write(image)

    print("output   : %s" % os.path.abspath(out_path))
    print("total    : %d  (header %d + firmware %d)"
          % (len(image), IMAGE_HEADER_SIZE, len(image) - IMAGE_HEADER_SIZE))
    print("link     : App window  -> factory burn address 0x%08X" % APP_BASE)
    print("OTA flow : host streams this image to Backup 0x08010000;")
    print("           BOOT copies Backup -> App 0x08004000 after verify")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Pack App firmware into a XATO image (single-App arch)")
    parser.add_argument("--bin", default=DEFAULT_BIN,
                        help="Keil raw bin path (default: qi_wireless_code_app "
                             "Objects/qi_wireless_code_app.bin)")
    parser.add_argument("--key", default=DEFAULT_KEY,
                        help="ECDSA private key PEM (default: docs/keys/private.pem)")
    parser.add_argument("--out-name", default=DEFAULT_OUT_NAME,
                        help="output file name inside app bin/ (default: %s)"
                             % DEFAULT_OUT_NAME)
    args = parser.parse_args(argv)

    priv = load_ec_private_key(args.key)
    return pack_one(args.bin, priv, args.out_name)


if __name__ == "__main__":
    sys.exit(main())
