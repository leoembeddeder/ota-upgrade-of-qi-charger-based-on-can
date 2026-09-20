# -*- coding: utf-8 -*-
"""把 Slot A Keil 裸 APP .bin 打成 256 字节 XATO 头 + 固件。

双 Target 编译：slotA（IROM1=0x08004100）+ slotB（IROM1=0x08010100）。OTA 脚本按目标槽自动选对应 bin。
产线：merge_prod_bin.py 把本输出烧到 0x08004000。

镜像头 0x4C 起 16B 为保留占位（原 version 字段已从 image_header_t 定义删除，
打包固定填 0x00，偏移锁定不可回收），打包产物不携带版本号；
版本号唯一定义在固件 SW_VERSION_STR（can_protocol.c），发版只改固件常量 + 文档。
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
    IMAGE_HEADER_SIZE,
    SLOT_A,
    SLOT_A_BASE,
    SLOT_B_BASE,
    load_ec_private_key,
    pack_image_if_needed,
    slot_name,
    validate_image,
)

REPO_ROOT = os.path.dirname(os.path.dirname(HERE))
APP_BIN_DIR = os.path.join(PARENT, "app bin")
DEFAULT_BIN_A = os.path.join(REPO_ROOT, "qi_wireless_code_slotA", "mdk_project", "Objects", "qi_wireless.bin")
DEFAULT_BIN_B = os.path.join(REPO_ROOT, "qi_wireless_code_slotB", "mdk_project", "Objects", "qi_wireless.bin")
DEFAULT_KEY = os.path.join(REPO_ROOT, "docs", "keys", "private.pem")


def pack_one(bin_path, priv):
    """打包单个槽的固件，返回 0=成功，1=失败。"""
    if not os.path.isfile(bin_path):
        print("跳过（找不到）: %s" % bin_path)
        return 1
    image = pack_image_if_needed(bin_path, priv)
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
    print("版本号不在镜像头（0x4C 为保留占位区，原 version 字段已删除，固定填 0x00），见固件 SW_VERSION_STR")
    print("")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Pack Keil APP .bin with XATO header (CRC32 + ECDSA P-256); 镜像头不携带版本号（0x4C 为保留占位区，原 version 字段已删除）"
    )
    parser.add_argument("--bin", default=None, help="Keil bin 路径（指定时忽略 --slot）")
    parser.add_argument("--slot", default="both", choices=["a", "b", "both"],
                        help="打包槽位: a=slotA(IROM1=0x08004100), b=slotB(IROM1=0x08010100), both=全部")
    parser.add_argument("--key", default=DEFAULT_KEY, help="ECDSA P-256 私钥 PEM（须与 Bootloader 公钥成对）")
    parser.add_argument("--out", default=None, help="输出镜像路径（仅 --bin 模式有效）")
    args = parser.parse_args(argv)

    if not os.path.isfile(args.key):
        sys.stderr.write("找不到私钥: %s\n" % args.key)
        return 1

    priv = load_ec_private_key(args.key)

    if args.bin is not None:
        # 指定单个 bin 路径
        if args.out is not None:
            image = pack_image_if_needed(args.bin, priv)
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
            print("版本号不在镜像头（0x4C 为保留占位区，原 version 字段已删除，固定填 0x00），见固件 SW_VERSION_STR")
            return 0
        return pack_one(args.bin, priv)
    # 按 --slot 批量打包（默认 both：slotA + slotB）
    rc = 0
    if args.slot in ("a", "both"):
        rc |= pack_one(DEFAULT_BIN_A, priv)
    if args.slot in ("b", "both"):
        rc |= pack_one(DEFAULT_BIN_B, priv)
    return rc


if __name__ == "__main__":
    sys.exit(main())
