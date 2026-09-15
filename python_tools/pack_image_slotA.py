# -*- coding: utf-8 -*-
"""把 Keil 裸 APP .bin 打成 256 字节 XATO 头 + 固件，供产线烧录或 OTA。

版本：1.1.1（写死在 image header 中）
用途：打包 Slot A 固件

产线：输出文件从槽起始地址烧录（Slot A = 0x08007000）。
OTA：输出到 python_tools/app bin/ 目录，供 zcanpro_ext_ota_auto.py 自动识别。

依赖：标准库 + 同目录 zcanpro_ext_ota_slotA.py（不需要 ZCANPRO / cryptography）。
"""

from __future__ import print_function

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

from zcanpro_ext_ota_slotA_1_1_1 import (  # noqa: E402
    IMAGE_HEADER_SIZE,
    SLOT_A,
    SLOT_A_BASE,
    SLOT_B_BASE,
    load_ec_private_key,
    pack_image_if_needed,
    slot_name,
    validate_image,
)

REPO_ROOT = os.path.dirname(HERE)
APP_BIN_DIR = os.path.join(HERE, "app bin")
DEFAULT_BIN_A = os.path.join(REPO_ROOT, "qi_wireless_code_slotA", "mdk_project", "Objects", "qi_wireless.bin")
DEFAULT_BIN_B = os.path.join(REPO_ROOT, "qi_wireless_code_slotB", "mdk_project", "Objects", "qi_wireless.bin")
DEFAULT_KEY = os.path.join(REPO_ROOT, "docs", "keys", "private.pem")

IMAGE_VERSION = "1.1.1"


def pack_one(bin_path, priv, version):
    """打包单个槽的固件，返回 0=成功，1=失败。"""
    if not os.path.isfile(bin_path):
        print("跳过（找不到）: %s" % bin_path)
        return 1
    image = pack_image_if_needed(bin_path, priv, version=version)
    linked = validate_image(image)

    if not os.path.isdir(APP_BIN_DIR):
        os.makedirs(APP_BIN_DIR)
    out_path = os.path.join(APP_BIN_DIR, "app_slot_%s.bin" % slot_name(linked).lower())

    with open(out_path, "wb") as f:
        f.write(image)

    burn_addr = "0x%08X" % (SLOT_A_BASE if linked == SLOT_A else SLOT_B_BASE)
    print("输出: %s" % os.path.abspath(out_path))
    print("总长: %d  (头 %d + 固件 %d)" % (len(image), IMAGE_HEADER_SIZE, len(image) - IMAGE_HEADER_SIZE))
    print("链接: Slot %s  → 产线烧录地址 %s" % (slot_name(linked), burn_addr))
    print("")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Pack Keil APP .bin with XATO header (CRC32 + ECDSA P-256) [v%s]" % IMAGE_VERSION
    )
    parser.add_argument("--bin", default=None, help="指定单个 Keil bin 路径（不指定则自动打包 Slot A + Slot B）")
    parser.add_argument("--key", default=DEFAULT_KEY, help="ECDSA P-256 私钥 PEM（须与 Bootloader 公钥成对）")
    parser.add_argument("--out", default=None, help="输出镜像路径（仅 --bin 模式有效）")
    args = parser.parse_args(argv)

    if not os.path.isfile(args.key):
        sys.stderr.write("找不到私钥: %s\n" % args.key)
        return 1

    priv = load_ec_private_key(args.key)

    if args.bin is not None:
        # 单槽模式
        if args.out is not None:
            image = pack_image_if_needed(args.bin, priv, version=IMAGE_VERSION)
            linked = validate_image(image)
            out_dir = os.path.dirname(os.path.abspath(args.out))
            if out_dir and not os.path.isdir(out_dir):
                os.makedirs(out_dir)
            with open(args.out, "wb") as f:
                f.write(image)
            burn_addr = "0x%08X" % (SLOT_A_BASE if linked == SLOT_A else SLOT_B_BASE)
            print("输出: %s" % os.path.abspath(args.out))
            print("总长: %d  (头 %d + 固件 %d)" % (len(image), IMAGE_HEADER_SIZE, len(image) - IMAGE_HEADER_SIZE))
            print("链接: Slot %s  → 产线烧录地址 %s" % (slot_name(linked), burn_addr))
            return 0
        else:
            return pack_one(args.bin, priv, IMAGE_VERSION)
    else:
        # 自动打包 Slot A + Slot B
        print("自动打包 Slot A + Slot B (version=%s) ..." % IMAGE_VERSION)
        print("")
        ret = 0
        ret |= pack_one(DEFAULT_BIN_A, priv, IMAGE_VERSION)
        ret |= pack_one(DEFAULT_BIN_B, priv, IMAGE_VERSION)
        return ret


if __name__ == "__main__":
    sys.exit(main())
