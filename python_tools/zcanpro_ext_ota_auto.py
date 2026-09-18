# -*- coding: utf-8 -*-
"""
ZCANPRO 扩展脚本 — Qi 无线充 CAN-UDS OTA

在 APP 内擦写非活跃槽（31/34/36/37），11 01 后由 Boot 切槽。
固件只编 Slot A（IROM1=0x08004100）；写入 B 时脚本自动重定位并重签。

导入: 高级功能 -> 扩展脚本 -> 打开本文件
运行前: 先打开 CAN 通道 (250 kbps, Classical CAN, 扩展帧)
需要: Python 3.8 32 位（ZCANPRO 扩展脚本要求）
固件: app bin/app_slot_a.bin，或 Slot A 的 qi_wireless.bin（现场打包）
"""

import os
import sys
import time
import struct
import hashlib
import binascii
import zlib

try:
    import zcanpro
except ImportError:
    zcanpro = None

# ======== 用户配置 ========
_TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(_TOOLS_DIR)
FIRMWARE_DIR = os.path.join(_TOOLS_DIR, "app bin")
PRIVATE_KEY_PATH = os.path.join(REPO_ROOT, "docs", "keys", "private.pem")
KEIL_BIN_A = os.path.join(REPO_ROOT, "qi_wireless_code_slotA", "mdk_project", "Objects", "qi_wireless.bin")

def _scan_firmware():
    """扫描 app bin/ 目录，返回 {SLOT_A: path, SLOT_B: path} 字典。"""
    result = {}
    if not os.path.isdir(FIRMWARE_DIR):
        raise RuntimeError("找不到固件目录: " + FIRMWARE_DIR)
    for name in sorted(os.listdir(FIRMWARE_DIR)):
        if not name.endswith(".bin"):
            continue
        path = os.path.join(FIRMWARE_DIR, name)
        data = open(path, "rb").read()
        if len(data) < IMAGE_HEADER_SIZE + 8:
            continue
        if struct.unpack_from("<I", data, 0)[0] == IMAGE_MAGIC:
            reset = struct.unpack_from("<I", data, IMAGE_HEADER_SIZE + 4)[0] & 0xFFFFFFFE
        else:
            reset = struct.unpack_from("<I", data, 4)[0] & 0xFFFFFFFE
        a0 = SLOT_A_BASE + IMAGE_HEADER_SIZE
        a1 = SLOT_A_BASE + SLOT_SIZE
        b0 = SLOT_B_BASE + IMAGE_HEADER_SIZE
        b1 = SLOT_B_BASE + SLOT_SIZE
        if a0 <= reset < a1:
            _log("扫描: %s → Slot A (Reset=0x%08X)" % (name, reset))
            result[SLOT_A] = path
        elif b0 <= reset < b1:
            _log("扫描: %s → Slot B (Reset=0x%08X)" % (name, reset))
            result[SLOT_B] = path
    return result


def _pick_firmware():
    """优先 app_slot_a.bin，其次 Slot A Keil 裸 bin，再扫 app bin/ 里任意 XATO。"""
    ordered = [os.path.join(FIRMWARE_DIR, "app_slot_a.bin"), KEIL_BIN_A]
    slots = {}
    try:
        slots = _scan_firmware()
    except RuntimeError:
        slots = {}
    for p in slots.values():
        if p not in ordered:
            ordered.append(p)
    for path in ordered:
        if os.path.isfile(path):
            _log("固件 " + path)
            return path
    raise RuntimeError("找不到固件。请编 Slot A 或运行 pack_image_slotA_1_1_1.py")

FIRMWARE_PATH = ""
DOWNLOAD_ADDR = 0x08004000
TRANSFER_BLOCK_DATA = 128

UDS_REQ_ID = 0x18DA0D03
UDS_RESP_ID = 0x18DA030D
SID_DSC, SID_ER, SID_RDBI, SID_SA = 0x10, 0x11, 0x22, 0x27
SID_WDBI, SID_RC, SID_RD, SID_TD, SID_RTE = 0x2E, 0x31, 0x34, 0x36, 0x37
SID_TP = 0x3E
SID_NRC, SID_PR = 0x7F, 0x40
NRC_RCRRP = 0x78
SA_SIG_CHUNK = 4  # 27 03 单帧：SID+03+seq+4B = 7，避开 ISO-TP 多帧
IMAGE_MAGIC = 0x4F544158
IMAGE_HEADER_SIZE = 256
SLOT_A, SLOT_B = 0, 1
SLOT_A_BASE = 0x08004000
SLOT_B_BASE = 0x08010000
SLOT_SIZE = 0xC000
MAX_TD_DATA = 254

# SIT1145 Standby 唤醒标识帧：固件唤醒后约 100ms（CAN_LP_ANNOUNCE_DELAY_MS）
# 在 0x18FF260D 主动发 01 41 57 4B cnt src secL secH（can_protocol.c can_lp_send_ident_bus），
# 与上电 BOOTUP 01 41 00 ... 区分。收到它 = 唤醒收敛的正证据。
LIFE_ANNOUNCE_ID = 0x18FF260D
LIFE_ANNOUNCE_MAGIC = (0x01, 0x41, 0x57, 0x4B)  # 01 'A' 'W' 'K'
LIFE_BOOTUP_MAGIC = (0x01, 0x41, 0x00)  # 01 'A' 00 — 上电/Boot 跳转标识帧
PROBE_ROUNDS = 3

# ---- Boot safe mode 诊断标记帧（与 qi_wireless_bootloader/mdk_app/Src/
# ---- boot_safe_mode.c 头部注释严格一致，两侧勿改其一）----
# 探测请求: CAN ID 0x18DA0D03 (UDS_REQ_ID)，数据 22 21 13
#           固件兼容 ISO-TP SF（03 22 21 13 ...）与裸 UDS（22 21 13）
# Boot safe mode 应答: CAN ID 0x18DA030D (UDS_RESP_ID)，5 字节原始单帧
#           （非 ISO-TP、无 PCI 字节，仅此一帧）:
#               62 21 13 FE <fail_step>
# fail_step 语义提取自 boot_verify.c g_verify_fail_step（1~6 为镜像校验
# 步骤，0 为 select_boot_slot 无有效槽/未执行校验）。
SAFE_MODE_RESP_ID = UDS_RESP_ID          # 0x18DA030D
SAFE_MODE_MARKER = (0x62, 0x21, 0x13, 0xFE)
FAIL_STEP_DESC = {
    0: "未执行镜像校验 / select_boot_slot 无有效槽（metadata 无 active/trial 槽）",
    1: "镜像 magic 校验失败",
    2: "image_length 为 0 或超出槽范围",
    3: "镜像 CRC32 校验失败",
    4: "Reset handler 不在槽内（跨槽链接镜像）",
    5: "ECDSA 公钥缺失/无效（Device Info 与内置公钥均不可用）",
    6: "ECDSA P-256 验签失败",
}

# secp256r1 / prime256v1. n 必须与 bootloader uECC.c 的 N[] 一致。
_P = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF
_N = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
_A = _P - 3
_GX = 0x6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296
_GY = 0x4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5

stopTask = False


class UdsNrcError(RuntimeError):
    def __init__(self, sid, nrc):
        RuntimeError.__init__(self, "NRC SID=0x%02X NRC=0x%02X" % (sid, nrc))
        self.sid = sid
        self.nrc = nrc


def _safe_mode_step(dat):
    """识别 Boot safe mode 标记帧 62 21 13 FE <fail_step>，返回 step 或 None。"""
    if (len(dat) >= 5 and dat[0] == 0x62 and dat[1] == 0x21
            and dat[2] == 0x13 and dat[3] == 0xFE):
        return int(dat[4])
    return None


def _safe_mode_msg(step):
    desc = FAIL_STEP_DESC.get(step, "未知 fail_step（Boot 固件可能早于本标记帧版本）")
    return ("设备处于 Boot safe mode，fail_step=%d（%s）→ merge_prod_bin 烧录器重刷"
            % (step, desc))


class SafeModeError(RuntimeError):
    """Boot safe mode 标记帧命中：设备停在 Boot，需 merge_prod_bin 重刷。"""
    def __init__(self, step):
        RuntimeError.__init__(self, _safe_mode_msg(step))
        self.step = step


def z_notify(type, obj):
    _log("Notify " + str(type) + " " + str(obj))
    if type == "stop":
        global stopTask
        stopTask = True


def _log(msg):
    text = str(msg)
    if zcanpro is not None:
        zcanpro.write_log(text)
    else:
        sys.stdout.write(text + "\n")
        sys.stdout.flush()


def _hex(data):
    if data is None:
        return ""
    return " ".join("%02X" % (int(b) & 0xFF) for b in data)


def _to_list(b):
    if sys.version_info[0] >= 3:
        return list(b)
    return [ord(c) for c in b]


def _to_bytes(seq):
    if isinstance(seq, bytes):
        return seq
    if sys.version_info[0] >= 3:
        return bytes(seq)
    return "".join(chr(int(x) & 0xFF) for x in seq)


def _u8(buf, i):
    v = buf[i]
    return v if isinstance(v, int) else ord(v)


def _int_be(b):
    if sys.version_info[0] >= 3:
        return int.from_bytes(b, "big")
    return int(binascii.hexlify(b), 16)


def _inv(x, m):
    x %= m
    if sys.version_info[0] >= 3:
        return pow(x, -1, m)
    return pow(x, m - 2, m)


def _jp_double(x, y, z):
    if z == 0 or y == 0:
        return 0, 0, 0
    ysq = (y * y) % _P
    s = (4 * x * ysq) % _P
    m = (3 * x * x + _A * ((z * z) % _P) * ((z * z) % _P)) % _P
    nx = (m * m - 2 * s) % _P
    ny = (m * (s - nx) - 8 * ysq * ysq) % _P
    nz = (2 * y * z) % _P
    return nx, ny, nz


def _jp_add(x1, y1, z1, x2, y2, z2):
    if z1 == 0:
        return x2, y2, z2
    if z2 == 0:
        return x1, y1, z1
    z1z1 = (z1 * z1) % _P
    z2z2 = (z2 * z2) % _P
    u1 = (x1 * z2z2) % _P
    u2 = (x2 * z1z1) % _P
    s1 = (y1 * z2 * z2z2) % _P
    s2 = (y2 * z1 * z1z1) % _P
    if u1 == u2:
        if (s1 + s2) % _P == 0:
            return 0, 0, 0
        return _jp_double(x1, y1, z1)
    h = (u2 - u1) % _P
    r = (s2 - s1) % _P
    h2 = (h * h) % _P
    h3 = (h * h2) % _P
    nx = (r * r - h3 - 2 * u1 * h2) % _P
    ny = (r * (u1 * h2 - nx) - s1 * h3) % _P
    nz = (h * z1 * z2) % _P
    return nx, ny, nz


def _jp_mul(k, x, y):
    rx, ry, rz = 0, 0, 0
    sx, sy, sz = x, y, 1
    while k > 0:
        if k & 1:
            rx, ry, rz = _jp_add(rx, ry, rz, sx, sy, sz)
        sx, sy, sz = _jp_double(sx, sy, sz)
        k >>= 1
    if rz == 0:
        return 0, 0
    zinv = _inv(rz, _P)
    z2 = (zinv * zinv) % _P
    return (rx * z2) % _P, (ry * z2 * zinv) % _P


def _i2b32(v):
    if v < 0 or v >= (1 << 256):
        raise ValueError("ECDSA 整数超出 32 字节: bit_length=%d" % v.bit_length())
    if sys.version_info[0] >= 3:
        return v.to_bytes(32, "big")
    return binascii.unhexlify("%064x" % v)


def ecdsa_sign_msg(priv, msg):
    h = hashlib.sha256(msg).digest()
    z = _int_be(h) % _N
    while True:
        k = _int_be(os.urandom(32)) % _N
        if k == 0:
            continue
        x, _y = _jp_mul(k, _GX, _GY)
        r = x % _N
        if r == 0:
            continue
        s = (_inv(k, _N) * (z + r * priv)) % _N
        if s == 0:
            continue
        return _i2b32(r) + _i2b32(s)


def _der_len(buf, i, end):
    if i >= end:
        raise ValueError("DER truncated")
    first = _u8(buf, i)
    i += 1
    if first < 0x80:
        return first, i
    n = first & 0x7F
    if n == 0 or n > 4 or i + n > end:
        raise ValueError("DER length")
    ln = 0
    for _k in range(n):
        ln = (ln << 8) | _u8(buf, i)
        i += 1
    return ln, i


def _collect_octet32(buf, out, start, end):
    i = start
    while i < end:
        tag = _u8(buf, i)
        i += 1
        try:
            ln, i = _der_len(buf, i, end)
        except ValueError:
            break
        if i + ln > end:
            break
        if tag == 0x04:
            if ln == 32:
                out.append(buf[i:i + 32])
            else:
                _collect_octet32(buf, out, i, i + ln)
        elif tag in (0x30, 0x31, 0xA0, 0xA1):
            _collect_octet32(buf, out, i, i + ln)
        i += ln


def load_ec_private_key(path):
    raw = open(path, "rb").read()
    if len(raw) == 32:
        priv = _int_be(raw)
        if 0 < priv < _N:
            return priv
    text = raw.decode("ascii", "ignore") if sys.version_info[0] >= 3 else raw
    if "BEGIN" in text:
        lines = []
        take = False
        for line in text.splitlines():
            s = line.strip()
            if "BEGIN" in s:
                take = True
                continue
            if "END" in s:
                break
            if take:
                lines.append(s)
        der = binascii.a2b_base64("".join(lines))
    else:
        der = raw
    cands = []
    _collect_octet32(der, cands, 0, len(der))
    for key in cands:
        if len(key) != 32:
            continue
        priv = _int_be(key)
        if 0 < priv < _N:
            return priv
    raise ValueError("无法解析私钥: " + path)


def image_target_slot(image):
    if len(image) < IMAGE_HEADER_SIZE + 8:
        return None
    reset = struct.unpack_from("<I", image, IMAGE_HEADER_SIZE + 4)[0] & 0xFFFFFFFE
    a0 = SLOT_A_BASE + IMAGE_HEADER_SIZE
    a1 = SLOT_A_BASE + SLOT_SIZE
    b0 = SLOT_B_BASE + IMAGE_HEADER_SIZE
    b1 = SLOT_B_BASE + SLOT_SIZE
    if a0 <= reset < a1:
        return SLOT_A
    if b0 <= reset < b1:
        return SLOT_B
    return None


def slot_name(slot):
    if slot == SLOT_A:
        return "A"
    if slot == SLOT_B:
        return "B"
    return "?"


def slot_base(slot):
    if slot == SLOT_B:
        return SLOT_B_BASE
    return SLOT_A_BASE


def validate_image(image):
    if len(image) < IMAGE_HEADER_SIZE + 8:
        raise RuntimeError("镜像太短: %d" % len(image))
    if struct.unpack_from("<I", image, 0)[0] != IMAGE_MAGIC:
        raise RuntimeError("镜像缺少 XATO 头")
    payload_len = struct.unpack_from("<I", image, 4)[0]
    expected = IMAGE_HEADER_SIZE + payload_len
    if expected != len(image):
        raise RuntimeError("镜像头 length=%d 与文件总长 %d 不一致" % (payload_len, len(image)))
    if len(image) > SLOT_SIZE:
        raise RuntimeError("镜像 %d 超过槽大小 %d (0x%X)" % (len(image), SLOT_SIZE, SLOT_SIZE))
    linked = image_target_slot(image)
    if linked is None:
        reset = struct.unpack_from("<I", image, IMAGE_HEADER_SIZE + 4)[0]
        raise RuntimeError("Reset Handler 0x%08X 不在 Slot A/B 内，请改 Target IROM1（A=0x08004100 / B=0x08010100）" % reset)
    _log("镜像链接 Slot %s, 总长 %d" % (slot_name(linked), len(image)))
    return linked

def relocate_image_to_slot(image, dest, priv):
    """Move a Slot-A-linked (or B-linked) image onto dest and re-sign.

    1.0 on A upgrading to 1.2 (also built as A) writes inactive B: every
    Flash pointer in the payload is shifted by (B-A) and CRC/ECDSA redone.
    """
    linked = image_target_slot(image)
    if linked is None:
        raise RuntimeError("无法识别镜像链接槽")
    if dest == linked:
        _log("镜像已按 Slot %s 链接，无需重定位" % slot_name(dest))
        return image
    delta = (slot_base(dest) - slot_base(linked)) & 0xFFFFFFFF
    lo = slot_base(linked)
    hi = lo + SLOT_SIZE
    payload = bytearray(image[IMAGE_HEADER_SIZE:])
    n = 0
    i = 0
    while i + 4 <= len(payload):
        w = struct.unpack_from("<I", payload, i)[0]
        raw = w & 0xFFFFFFFE
        if lo <= raw < hi:
            struct.pack_into("<I", payload, i, ((raw + delta) & 0xFFFFFFFE) | (w & 1))
            n += 1
        i += 4
    payload = bytes(payload)
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    sig = ecdsa_sign_msg(priv, payload)
    # 镜像头 version 区（0x4C..0x5C，16B）不再携带版本号，重定位重签时固定填 0x00。
    # 版本号唯一定义在固件 SW_VERSION_STR（can_protocol.c）；签名/CRC 只覆盖头后
    # payload，此字段取值不影响校验。旧镜像头里的历史版本字节在此被清零。
    ver = b"\x00" * 16
    ts = image[92:96]
    header = struct.pack("<III", IMAGE_MAGIC, len(payload), crc) + sig + ver + ts
    header += b"\x00" * (IMAGE_HEADER_SIZE - len(header))
    _log("镜像 Slot %s → Slot %s，重定位 %d 处地址 crc=0x%08X" % (
        slot_name(linked), slot_name(dest), n, crc))
    return header + payload



def pack_image_if_needed(fw_path, priv):
    """必要时为裸 bin 补 XATO 头。镜像头 version 区（0x4C）固定填 0x00，
    打包产物不携带版本号；版本号唯一定义在固件 SW_VERSION_STR
    （can_protocol.c），发版只改固件常量 + 文档。CRC32/ECDSA 签名只覆盖
    头后 payload，version 字段不参与任何校验。"""
    data = open(fw_path, "rb").read()
    if len(data) >= IMAGE_HEADER_SIZE and struct.unpack_from("<I", data, 0)[0] == IMAGE_MAGIC:
        _log("固件已带 XATO 头, 总长 %d" % len(data))
        return data
    packed_len = IMAGE_HEADER_SIZE + len(data)
    if packed_len > SLOT_SIZE:
        raise RuntimeError("裸 bin %d + 头 256 = %d，超过槽大小 %d" % (len(data), packed_len, SLOT_SIZE))
    _log("固件无头，现场打包 %d 字节" % len(data))
    crc = zlib.crc32(data) & 0xFFFFFFFF
    sig = ecdsa_sign_msg(priv, data)
    # 镜像头 version 区固定 0x00：打包产物不携带版本号
    ver = b"\x00" * 16
    header = struct.pack("<III", IMAGE_MAGIC, len(data), crc) + sig + ver + struct.pack("<I", int(time.time()) & 0xFFFFFFFF)
    header += b"\x00" * (IMAGE_HEADER_SIZE - len(header))
    _log("打包完成 crc=0x%08X（版本号不在镜像头，见固件 SW_VERSION_STR）" % crc)
    return header + data


def _uds_init_cfg():
    return {
        "src_addr": UDS_REQ_ID,
        "dst_addr": UDS_RESP_ID,
        "response_timeout_ms": 3000,
        "use_canfd": 0,
        "canfd_brs": 0,
        "trans_ver": 0,
        "fill_byte": 0xCC,
        "frame_type": 1,
        "trans_stmin_valid": 1,
        "trans_stmin": 1,
        "enhanced_timeout_ms": 120000,
    }


def uds_init():
    zcanpro.uds_init(_uds_init_cfg())
    _log("UDS 就绪 0x18DA0D03 / 0x18DA030D 扩展帧")


def uds_req(bus_id, sid, payload, suppress=0, wait_pending_s=0):
    """NRC 0x78 is not final.

    注意（实测定性）：本机 ZCANPRO 库内部消化 NRC 0x78（不把 7F xx 78 当返回
    数据抛给脚本），并把 enhanced_timeout_ms 当绝对上限计时。因此下面
    wait_pending_s>0 的 0x78 重试分支在本库上可能不触发。0x31 仍走
    uds_request，enhanced_timeout_ms=120s 覆盖擦槽。
    """
    if stopTask:
        raise RuntimeError("用户停止脚本")
    req = {
        "src_addr": UDS_REQ_ID,
        "dst_addr": UDS_RESP_ID,
        "suppress_response": 1 if suppress else 0,
        "sid": sid,
        "data": list(payload),
    }
    t_end = time.time() + float(wait_pending_s)
    logged_tx = False
    while True:
        if stopTask:
            raise RuntimeError("用户停止脚本")
        if not logged_tx:
            _log("[Tx] %02X %s%s" % (sid, _hex(payload[:16]) + (" ..." if len(payload) > 16 else ""),
                                     " (suppress)" if suppress else ""))
            logged_tx = True
        resp = zcanpro.uds_request(bus_id, req)
        if suppress:
            return None
        data = list((resp or {}).get("data") or [])
        if data:
            _log("[Rx] " + _hex(data[:24]))
            sm_step = _safe_mode_step(data)
            if sm_step is not None:
                # 库若把 Boot safe mode 标记帧当响应数据透传（62 21 13 FE <step>），
                # 立即按 safe mode 报错，不得误判为 APP DID 0x2113 正响应。
                raise SafeModeError(sm_step)
        if len(data) >= 3 and data[0] == SID_NRC:
            if data[2] == NRC_RCRRP:
                if wait_pending_s <= 0 or time.time() >= t_end:
                    raise RuntimeError("SID=0x%02X 只收到 NRC 0x78，ZCANPRO 未等到最终响应" % sid)
                _log("SID=0x%02X NRC 0x78，MCU 忙，继续等待" % sid)
                time.sleep(1.0)
                continue
            raise UdsNrcError(data[1], data[2])
        if not resp or not resp.get("result"):
            raise RuntimeError("无应答 SID=0x%02X %s" % (sid, (resp or {}).get("result_msg", "")))
        if len(data) < 1 or data[0] != (sid + SID_PR):
            raise RuntimeError("非正响应 SID=0x%02X %s" % (sid, _hex(data)))
        return data


def uds_try(bus_id, sid, payload, suppress=0):
    try:
        return uds_req(bus_id, sid, payload, suppress=suppress)
    except Exception as e:
        _log("可忽略: " + str(e))
        return None


def uds_ecu_reset(bus_id):
    """0x11 with SPR=1: MCU resets, no 0x51. Do not wait the 3s UDS timeout."""
    uds_try(bus_id, SID_ER, [0x81], suppress=1)


def read_did_u8(bus_id, did):
    rx = uds_req(bus_id, SID_RDBI, [(did >> 8) & 0xFF, did & 0xFF])
    if len(rx) < 4:
        raise RuntimeError("DID 0x%04X 响应过短" % did)
    return rx[3]


def send_security_key(bus_id, sig, seed=None, priv=None):
    """27 02 + 64-byte key. On 0x24 re-request seed and retry once.
    27 03 only if 27 02 returns 0x12/0x13 (old APP without one-shot key)."""
    sig = _to_bytes(sig)
    if len(sig) != 64:
        raise RuntimeError("ECDSA 签名须 64 字节, 实际 %d" % len(sig))
    time.sleep(0.08)
    try:
        _log("SendKey 27 02 + 64 字节")
        return uds_req(bus_id, SID_SA, [0x02] + _to_list(sig), wait_pending_s=45)
    except UdsNrcError as e:
        if e.nrc == 0x24 and seed is not None and priv is not None:
            _log("27 02 NRC 0x24，重新 27 01 再送 27 02")
            time.sleep(0.1)
            rx = uds_req(bus_id, SID_SA, [0x01])
            if len(rx) >= 34:
                seed2 = _to_bytes(rx[2:34])
                if seed2 != b"\x00" * 32:
                    sig = ecdsa_sign_msg(priv, seed2)
            time.sleep(0.08)
            return uds_req(bus_id, SID_SA, [0x02] + _to_list(sig), wait_pending_s=45)
        if e.nrc not in (0x12, 0x13):
            raise
        _log("27 02 整包 NRC 0x%02X，改 27 03 分片" % e.nrc)
    seq = 1
    off = 0
    while off < 64:
        piece = sig[off:off + SA_SIG_CHUNK]
        uds_req(bus_id, SID_SA, [0x03, seq] + _to_list(piece))
        off += len(piece)
        seq += 1
    _log("27 03 已送 64 字节 / %d 帧" % (seq - 1))
    time.sleep(0.15)
    return uds_req(bus_id, SID_SA, [0x02], wait_pending_s=45)


def _lifecycle_check(cid, dat):
    """判断 0x18FF260D 上的帧类型：awk/bootup/shutdown/None。"""
    if cid != LIFE_ANNOUNCE_ID or len(dat) < 3:
        return None
    if len(dat) >= 4 and tuple(dat[:4]) == LIFE_ANNOUNCE_MAGIC:
        return "awk"          # 01 41 57 4B — 从 Standby 唤醒
    if dat[0] == 0x01 and dat[1] == 0x41 and dat[2] == 0x00:
        return "bootup"       # 01 41 00 — 上电/Boot 跳转
    if dat[0] == 0x06 and dat[1] == 0x41:
        return "shutdown"     # 06 41 53 42 — 进入 Standby
    return None


def _listen_lifecycle(bus_id, listen_s=1.0):
    """释放 UDS 通道后 raw 收帧，监听 0x18FF260D 生命周期帧。
    返回 [(类型, data), ...]；finally 恢复 UDS 通道。"""
    found = []
    try:
        zcanpro.uds_deinit()
    except Exception as e:
        _log("UDS 通道释放失败（继续监听）: " + str(e))
    try:
        t_end = time.time() + float(listen_s)
        while time.time() < t_end:
            if stopTask:
                raise RuntimeError("用户停止脚本")
            for cid, dat in _recv_frames(bus_id):
                lt = _lifecycle_check(cid, dat)
                if lt is not None:
                    _log("生命周期帧 [%s] 0x%08X %s" % (lt.upper(), cid, _hex(dat[:8])))
                    found.append((lt, dat))
            time.sleep(0.02)
    finally:
        uds_init()
    return found


ERASE_REQ = [0x01, 0xFF, 0x00]
ERASE_WAIT_PENDING_S = 90
ERASE_ENHANCED_CFG_S = 120  # _uds_init_cfg() enhanced_timeout_ms=120000（配置值）


def _sniff_erase_late_response(bus_id, sniff_s=3.0):
    """0x31 擦除 uds_request 失败后的原始通道取证。

    本机 ZCANPRO 库内部消化 0x78（脚本在 uds_request 路径下看不到泵帧），
    且实测失败耗时 55s 既不等于配置的 enhanced_timeout 120s 也不等于
    response_timeout 3s，库内部另有 RCRRP/无响应预算，语义不可从 WSL 取证。
    失败后释放 UDS 通道 raw 收帧，抓迟到的最终响应：
      ("positive", 0)  → MCU 擦除实际完成，0x71 迟到被库放弃
      ("nrc", code)    → MCU 回了最终 NRC（设备存活，固件拒绝）
      None             → 全静默（MCU 挂死或 CAN TX 黑洞）
    顺带统计嗅探窗口内的 7F 31 78 泵帧数，日志可区分「MCU 一直在泵 78
    但库提前放弃」与「MCU 一个帧都没发出」。finally 恢复 UDS 通道。"""
    pend = 0
    result = None
    try:
        zcanpro.uds_deinit()
    except Exception as e:
        _log("UDS 通道释放失败（继续嗅探）: " + str(e))
    try:
        t_end = time.time() + float(sniff_s)
        while time.time() < t_end:
            if stopTask:
                raise RuntimeError("用户停止脚本")
            for cid, dat in _recv_frames(bus_id):
                if (cid != UDS_RESP_ID) or (len(dat) < 2):
                    continue
                if dat[0] == SID_NRC and len(dat) >= 4 and dat[2] == SID_RC:
                    if dat[3] == NRC_RCRRP:
                        pend += 1
                        continue
                    _log("原始通道捕获擦除最终 NRC: " + _hex(dat[:4]))
                    result = ("nrc", dat[3])
                elif dat[0] == 0x71 and len(dat) >= 3 and dat[1] == 0x01:
                    _log("原始通道捕获擦除迟到正响应: " + _hex(dat[:8]))
                    result = ("positive", 0)
            time.sleep(0.02)
    finally:
        uds_init()
    _log("擦除失败取证：嗅探 %.1fs，捕获 0x78 泵帧 %d 个，最终响应 %s"
         % (sniff_s, pend, ("有" if result else "无")))
    return result


def _erase_with_retry(bus_id):
    """0x31 擦除带韧性：失败后取证 + 自动重试一次 + 耗时定性报错。

    固件侧擦除幂等且有活跃槽防护（g_base==ota_running_slot_base() 回 NRC 22），
    重试安全：MCU 若只是慢/瞬时 bus-off，二次 31 01 FF 00 可直接成功；
    MCU 若已实际擦完（0x71 迟到被库放弃），取证命中正响应则跳过重试续跑。"""
    t0 = time.time()
    try:
        uds_req(bus_id, SID_RC, ERASE_REQ, wait_pending_s=ERASE_WAIT_PENDING_S)
        return
    except UdsNrcError as e:
        _log("0x31 擦除耗时 %.1fs，MCU 回 NRC 0x%02X（设备存活，固件拒绝，重试无意义）"
             % (time.time() - t0, e.nrc))
        raise
    except RuntimeError as e:
        elapsed = time.time() - t0
        _log("0x31 擦除第 1 次失败：耗时 %.1fs「%s」（wait_pending 上限 %ds，"
             "库 enhanced_timeout 配置 %ds；0x78 被库内部消化，脚本侧全程不可见）"
             % (elapsed, e, ERASE_WAIT_PENDING_S, ERASE_ENHANCED_CFG_S))
        sniff = _sniff_erase_late_response(bus_id)
        if sniff and sniff[0] == "positive":
            _log("MCU 擦除实际已完成（0x71 迟到被库放弃），跳过重试直接续跑")
            return
        if sniff and sniff[0] == "nrc":
            raise RuntimeError("0x31 擦除被 MCU 拒绝：NRC 0x%02X（第 1 次耗时 %.1fs，"
                               "原始通道捕获最终 NRC，设备存活）"
                               % (sniff[1], elapsed))
        _log("原始通道无最终响应，自动重试一次 31 01 FF 00（固件擦除幂等+活跃槽防护，安全）")
        t1 = time.time()
        try:
            uds_req(bus_id, SID_RC, ERASE_REQ, wait_pending_s=ERASE_WAIT_PENDING_S)
            _log("0x31 擦除第 2 次成功（耗时 %.1fs）" % (time.time() - t1))
            return
        except Exception as e2:
            elapsed2 = time.time() - t1
            total = time.time() - t0
            probe = uds_try(bus_id, SID_RDBI, [0x21, 0x13])
            if probe:
                alive = "应答正常（%s），设备存活" % _hex(probe[:8])
            else:
                sm_step = _raw_probe_safe_mode(bus_id)
                if sm_step is not None:
                    alive = _safe_mode_msg(sm_step)
                else:
                    alive = "无应答（MCU 疑似挂死或 CAN TX 黑洞）"
            raise RuntimeError(
                "0x31 擦除两次失败：第1次 %.1fs「%s」，第2次 %.1fs「%s」，总计 %.1fs；"
                "wait_pending 上限 %ds，库 enhanced_timeout 配置 %ds"
                "（库内部消化 0x78 且另有上限，实测 55s 量级）。失败后 22 2113 探测：%s。"
                "两次耗时都贴近库内部上限→MCU 擦除慢于库预算；"
                "第2次远小于上限且探测无应答→MCU 擦除路径挂死/TX 黑洞，需固件修复后重编译烧录。"
                % (elapsed, e, elapsed2, e2, total,
                   ERASE_WAIT_PENDING_S, ERASE_ENHANCED_CFG_S, alive))


def confirm_app_after_reset(bus_id):
    """复位后等待 APP 起来。Boot 验签 ECDSA 需数秒，回退路径更久。

    三阶段：前 3 次盲探 22 2113 → 失败后 wake_bus + 监听生命周期帧 →
    继续探测并间歇监听。窗口 45s。

    三态诊断：
    a) UDS 响应 = 成功；
    b) 生命周期帧但无 UDS = APP 已启动但链路/会话异常；
    c) 全静默 = 可能停在 Boot safe mode 或镜像问题：raw 探测 22 2113 识别
       标记帧 62 21 13 FE <fail_step>（命中→SafeModeError 精确报错），
       未命中提示 merge_prod_bin。标记帧格式与 fail_step 语义见文件头
       SAFE_MODE 注释（与 boot_safe_mode.c 两侧一致）。
    """
    WINDOW_S = 45.0
    BLIND_PROBES = 3
    rx = None
    last_err = None
    t0 = time.time()
    probe_count = 0
    woken = False
    lifecycle_seen = []

    _log("等待 APP 起来（Boot 验签+可能回退，窗口 %.0fs）" % WINDOW_S)

    while time.time() - t0 < WINDOW_S:
        if stopTask:
            raise RuntimeError("用户停止脚本")

        # Phase 1: blind probe (no wake frames yet)
        if probe_count < BLIND_PROBES:
            probe_count += 1
            try:
                rx = uds_req(bus_id, SID_RDBI, [0x21, 0x13])
                last_err = None
                break
            except SafeModeError:
                raise
            except Exception as e:
                last_err = e
                _log("复位后 22 2113 盲探 %d/%d: %s" % (probe_count, BLIND_PROBES, e))
                sm_step = _raw_probe_safe_mode(bus_id)
                if sm_step is not None:
                    raise SafeModeError(sm_step)
                time.sleep(0.5)
                continue

        # Phase 2: wake bus + listen for lifecycle frames (once)
        if not woken:
            _log("盲探 %d 次无应答，唤醒总线并监听生命周期帧..." % BLIND_PROBES)
            woken = True
            try:
                wake_ok = wake_bus(bus_id, listen_s=2.0)
                _log("wake_bus: %s" % ("收到 AWK，已从 Standby 唤醒" if wake_ok
                                       else "未收到 AWK，继续探测"))
            except Exception as e:
                _log("wake_bus 异常: %s" % e)
            lifecycle_seen.extend(_listen_lifecycle(bus_id, listen_s=1.0))
            sm_step = _raw_probe_safe_mode(bus_id)
            if sm_step is not None:
                raise SafeModeError(sm_step)

        # Phase 3: probe + brief lifecycle listen between attempts
        try:
            rx = uds_req(bus_id, SID_RDBI, [0x21, 0x13])
            last_err = None
            break
        except SafeModeError:
            raise
        except Exception as e:
            last_err = e
            _log("复位后 22 2113 等待: %s" % e)
            lifecycle_seen.extend(_listen_lifecycle(bus_id, listen_s=0.5))
            sm_step = _raw_probe_safe_mode(bus_id)
            if sm_step is not None:
                raise SafeModeError(sm_step)
            time.sleep(0.5)

    if last_err is not None:
        if lifecycle_seen:
            details = "; ".join("%s %s" % (lt.upper(), _hex(dat[:8]))
                                for lt, dat in lifecycle_seen[:4])
            raise RuntimeError(
                "复位后 APP 已启动（生命周期帧: %s）但 UDS 22 2113 无应答"
                "——链路/会话异常，请检查 CAN 配置或会话状态: %s"
                % (details, last_err))
        else:
            sm_step = _raw_probe_safe_mode(bus_id)
            if sm_step is not None:
                raise SafeModeError(sm_step)
            raise RuntimeError(
                "复位后无 UDS 且无生命周期帧（%.0fs 全静默）"
                "——可能停在 Boot（验签失败进 safe mode / 镜像问题），"
                "safe-mode 标记帧也未捕获（旧 Boot 固件无此应答或 CAN 未起）；"
                "请用 merge_prod_bin.py 重刷排查: %s" % (WINDOW_S, last_err))

    _log("复位后 DID 0x2113 slot=" + _hex((rx or [])[3:4]))
    try:
        fw = uds_req(bus_id, SID_RDBI, [0x20, 0x10])
        _log("DID 0x2010 fw_type=" + _hex(fw[3:4]))
    except UdsNrcError as e:
        _log("DID 0x2010 NRC 0x%02X（Boot 无此 DID）" % e.nrc)
    # 运行版本确认：DID 0xF195 应答 = APP 编译时常量 SW_VERSION_STR
    # （can_protocol.c 唯一真相源），不读 OTA metadata / XATO 镜像头；
    # 解析方式与 zcanpro_read_app_version.py 一致（rx[3:35] 32B ASCII rstrip）。
    try:
        ver = uds_req(bus_id, SID_RDBI, [0xF1, 0x95])
        if len(ver) >= 35 and ver[0] == (SID_RDBI + SID_PR):
            sw = "".join(chr(b) if 0x20 <= b < 0x7F else "?" for b in ver[3:35]).rstrip()
            _log("DID 0xF195 APP编译版本=" + sw + "（来源=固件 SW_VERSION_STR 编译常量）")
        else:
            _log("DID 0xF195 响应异常: " + _hex(ver[:8]))
    except UdsNrcError as e:
        _log("DID 0xF195 NRC 0x%02X（不影响判定）" % e.nrc)
    except Exception as e:
        _log("DID 0xF195 读取失败（不影响判定）: %s" % e)
    try:
        uds_req(bus_id, SID_RD, [0x00])
        _log("复位后 0x34 正响应，已在 APP")
    except UdsNrcError as e:
        _log("复位后 0x34 NRC 0x%02X，已在 APP（默认会话下正常）" % e.nrc)
    except RuntimeError as e:
        raise RuntimeError("复位后 0x34 无应答（跳转失败或 APP CAN 未起来）: " + str(e))


def _as_int(x):
    try:
        return int(x)
    except Exception:
        return None


def _parse_can_frame(f):
    """dict / list / tuple / object → (can_id_29bit, data list) or None。
    解析方式与 zcanpro_read_app_version.py 实测一致。"""
    if f is None:
        return None
    if isinstance(f, dict):
        cid = None
        for k in ("can_id", "id", "CANID", "canid"):
            if k in f:
                cid = _as_int(f[k])
                break
        dat = f.get("data")
    elif isinstance(f, (list, tuple)) and len(f) >= 2 and _as_int(f[0]) is not None:
        cid = _as_int(f[0])
        dat = f[1]
    else:
        cid = _as_int(getattr(f, "can_id", getattr(f, "id", None)))
        dat = getattr(f, "data", None)
    if cid is None:
        return None
    if not isinstance(dat, (list, tuple, bytes, bytearray)):
        dat = []
    return (cid & 0x1FFFFFFF, [int(x) & 0xFF for x in list(dat)])


def _unwrap_receive(raw):
    """与 zcanpro_read_app_version.py 一致：receive 常见 (status, [frames])。"""
    if raw is None:
        return []
    if isinstance(raw, tuple) or (isinstance(raw, list) and len(raw) == 2
                                  and not isinstance(raw[0], dict)
                                  and isinstance(raw[1], (list, tuple))):
        a, b = raw[0], raw[1]
        if isinstance(b, (list, tuple)):
            return list(b)
        if isinstance(a, (list, tuple)):
            return list(a)
    if isinstance(raw, dict) or (not isinstance(raw, (list, tuple))):
        return [raw]
    return list(raw)


def _recv_frames(bus_id):
    try:
        raw = zcanpro.receive(bus_id)
    except TypeError:
        try:
            raw = zcanpro.receive()
        except Exception:
            return []
    except Exception:
        return []
    out = []
    for f in _unwrap_receive(raw):
        p = _parse_can_frame(f)
        if p is not None:
            out.append(p)
    return out


def _raw_send(bus_id, can_id, data):
    """扩展帧 raw 发送（ZLG bit31=1，参考 zcanpro_read_app_version.py
    实测模式）。仅用于 safe-mode 取证探测，不进 UDS 请求主路径
    （qi-can-uds-ota-scripts §3：raw 模式仅限取证/唤醒监听）。"""
    cid = (int(can_id) & 0x1FFFFFFF) | 0x80000000
    frame = {
        "can_id": cid,
        "id": cid,
        "is_canfd": 0,
        "canfd_brs": 0,
        "is_extend": 1,
        "is_extended": 1,
        "extend": 1,
        "extern_flag": 1,
        "is_extern": 1,
        "eff": 1,
        "id_type": 1,
        "data": list(data) + [0xCC] * (8 - len(data)),
    }
    for name in ("transmit", "send"):
        fn = getattr(zcanpro, name, None)
        if fn is None:
            continue
        try:
            fn(bus_id, [frame])
            return True
        except Exception:
            try:
                fn(bus_id, frame)
                return True
            except Exception:
                continue
    return False


def _raw_probe_safe_mode(bus_id, sniff_s=2.0):
    """失败路径取证：raw 发 22 2113 探测帧（ISO-TP SF）后监听 Boot safe
    mode 标记帧 62 21 13 FE <fail_step>（5 字节原始单帧，非 ISO-TP）。
    返回 fail_step（int）或 None；finally 恢复 UDS 通道。
    标记帧仅在设备收到探测帧时应答一次，库路径可能吞掉，故须 raw 重探。"""
    step = None
    try:
        zcanpro.uds_deinit()
    except Exception as e:
        _log("UDS 通道释放失败（继续 safe-mode 取证）: " + str(e))
    try:
        if _raw_send(bus_id, UDS_REQ_ID, [0x03, 0x22, 0x21, 0x13,
                                          0xCC, 0xCC, 0xCC, 0xCC]):
            _log("[Tx-raw] 0x%08X 03 22 21 13 (safe-mode 探测)" % UDS_REQ_ID)
        t_end = time.time() + float(sniff_s)
        while time.time() < t_end:
            if stopTask:
                raise RuntimeError("用户停止脚本")
            for cid, dat in _recv_frames(bus_id):
                if (cid & 0x1FFFFFFF) == SAFE_MODE_RESP_ID:
                    step = _safe_mode_step(dat)
                    if step is not None:
                        _log("[Rx-raw] 0x%08X %s → Boot safe mode fail_step=%d"
                             % (cid, _hex(dat[:8]), step))
                        return step
            time.sleep(0.02)
    finally:
        uds_init()
    return step


def wake_bus(bus_id, listen_s=2.0):
    """SIT1145 空闲 180s 进 Standby：首帧只当 WUP，MCU 收不到内容，
    所以唤醒帧必须连发、且发完要等固件收敛再探测。

    步骤：
    1. 连发 3 帧 3E 80（suppress，间隔 200ms）打破静默；
    2. 释放 UDS 通道后原始收帧监听 1~2s（zcanpro_read_app_version.py
       验证过：UDS 占用时 raw receive 不可靠，先 uds_deinit）；
    3. 收到 0x18FF260D 上的 01 41 57 4B 唤醒标识帧 = 唤醒成功，返回 True；
       超时未见标识帧返回 False（不能当作已唤醒）。"""
    if stopTask:
        raise RuntimeError("用户停止脚本")
    for _ in range(3):
        uds_try(bus_id, SID_TP, [0x80], suppress=1)
        time.sleep(0.2)
    try:
        zcanpro.uds_deinit()
    except Exception as e:
        _log("UDS 通道释放失败（继续监听）: " + str(e))
    try:
        t_end = time.time() + float(listen_s)
        while time.time() < t_end:
            if stopTask:
                raise RuntimeError("用户停止脚本")
            for cid, dat in _recv_frames(bus_id):
                if cid == LIFE_ANNOUNCE_ID and tuple(dat[:4]) == LIFE_ANNOUNCE_MAGIC:
                    _log("收到唤醒标识帧 0x%08X %s，总线已唤醒" % (cid, _hex(dat[:8])))
                    return True
            time.sleep(0.02)
        _log("监听 %.1fs 未见 0x%08X 唤醒标识帧" % (listen_s, LIFE_ANNOUNCE_ID))
        return False
    finally:
        uds_init()  # 恢复 UDS 通道，供后续探测/升级使用


def probe_in_app(bus_id, retries=PROBE_ROUNDS):
    """Boot 无 UDS（safe mode 除外，见下）。APP 在线则 22 2113 有应答
    （正响应或 NRC）。

    不要用 0x34 探测：APP 已实现下载，默认会话回 0x22；且 Standby 下首帧
    只当 WUP，3s 超时后 ident 早已发出，再去听 0x18FF260D 会漏。
    无应答时：① raw 取证探测 Boot safe mode 标记帧（命中→SafeModeError，
    报错带 fail_step 语义，不再笼统"无应答"）；② 连发 3E 80 再立刻重试
    2113（MCU 此时应已 Normal）；③ 全部轮次失败后 wake_bus + 生命周期帧
    监听再判一次（禁止纯裸探循环），唤醒后再探。"""
    for attempt in range(1, retries + 1):
        try:
            rx = uds_req(bus_id, SID_RDBI, [0x21, 0x13])
            _log("探测 22 2113 成功 slot=%s，当前在 APP" % _hex((rx or [])[3:4]))
            return True
        except SafeModeError:
            raise
        except UdsNrcError as e:
            _log("探测 22 2113 NRC 0x%02X，当前在 APP" % e.nrc)
            return True
        except RuntimeError as e:
            _log("探测 22 2113 无应答（第 %d/%d 轮）: %s" % (attempt, retries, e))
            sm_step = _raw_probe_safe_mode(bus_id)
            if sm_step is not None:
                raise SafeModeError(sm_step)
            if attempt < retries:
                for _ in range(3):
                    uds_try(bus_id, SID_TP, [0x80], suppress=1)
                    time.sleep(0.15)
                time.sleep(0.4)
    # 全部轮次无应答：唤醒 + 生命周期帧监听后再判（Standby 首帧只当 WUP）
    try:
        wake_ok = wake_bus(bus_id, listen_s=2.0)
        life = _listen_lifecycle(bus_id, listen_s=1.0)
        _log("wake_bus=%s，生命周期帧 %d 个" % (wake_ok, len(life)))
        if wake_ok or life:
            try:
                rx = uds_req(bus_id, SID_RDBI, [0x21, 0x13])
                _log("唤醒后探测 22 2113 成功 slot=%s，当前在 APP"
                     % _hex((rx or [])[3:4]))
                return True
            except SafeModeError:
                raise
            except Exception as e:
                _log("唤醒后探测 22 2113 仍无应答: %s" % e)
    except SafeModeError:
        raise
    except Exception as e:
        _log("唤醒/监听异常: %s" % e)
    sm_step = _raw_probe_safe_mode(bus_id)
    if sm_step is not None:
        raise SafeModeError(sm_step)
    return False


def run_ota(bus_id):
    global FIRMWARE_PATH
    if not (1 <= TRANSFER_BLOCK_DATA <= MAX_TD_DATA):
        raise RuntimeError("TRANSFER_BLOCK_DATA 须为 1..%d" % MAX_TD_DATA)
    if not os.path.isfile(PRIVATE_KEY_PATH):
        raise RuntimeError("找不到私钥: " + PRIVATE_KEY_PATH)
    _log("私钥 " + PRIVATE_KEY_PATH)
    priv = load_ec_private_key(PRIVATE_KEY_PATH)
    uds_init()
    try:
        FIRMWARE_PATH = _pick_firmware()
        image = pack_image_if_needed(FIRMWARE_PATH, priv)
        linked = validate_image(image)
        if not probe_in_app(bus_id):
            raise RuntimeError("UDS 无应答（已重试 %d 轮）。"
                               "跳转后试运行确认会擦 metadata，CAN 可能 bus-off；"
                               "请烧录含 bus-off 恢复的 APP 后再连升。"
                               "空片用 merge_prod_bin.py。" % PROBE_ROUNDS)
        _log("镜像链接 Slot %s；将写入非活跃槽（必要时重定位）" % slot_name(linked))
        _log("在 APP 内升级（31/34/36/37），完成后 11 01 由 Boot 切槽")
        _log("---- Programming ----")
        last_err = None
        for attempt in range(1, 9):
            try:
                uds_req(bus_id, SID_DSC, [0x02])
                last_err = None
                break
            except Exception as e:
                last_err = e
                _log("Programming 10 02 第 %d/8 次失败: %s" % (attempt, e))
                if attempt < 8:
                    time.sleep(0.5)
        if last_err is not None:
            raise last_err
        _log("---- SecurityAccess ----")
        rx = None
        last_sa = None
        for sa_try in range(1, 4):
            try:
                rx = uds_req(bus_id, SID_SA, [0x01])
                last_sa = None
                break
            except RuntimeError as e:
                last_sa = e
                _log("27 01 第 %d/3 次无应答: %s" % (sa_try, e))
                time.sleep(0.4)
        if last_sa is not None:
            raise last_sa
        if len(rx) < 6:
            raise RuntimeError("seed 响应过短")
        if len(rx) >= 34:
            seed = _to_bytes(rx[2:34])
            unlocked = (seed == b"\x00" * 32)
        else:
            seed = _to_bytes(rx[2:6])
            unlocked = (seed == b"\x00\x00\x00\x00")
        if unlocked:
            _log("已解锁 (ISO 14229 seed=0)，跳过 SendKey")
        else:
            if len(rx) < 34:
                raise RuntimeError("seed 须 32 字节, 实际 %d" % (len(rx) - 2))
            _log("seed " + _hex(rx[2:34]))
            sig = ecdsa_sign_msg(priv, seed)
            _log("SendKey 签名 %d 字节" % len(sig))
            send_security_key(bus_id, sig, seed=seed, priv=priv)
        _log("---- DID 0x2010 APP ----")
        uds_req(bus_id, SID_WDBI, [0x20, 0x10, 0x01])
        _log("---- 擦除 ----")
        _erase_with_retry(bus_id)
        dest = read_did_u8(bus_id, 0x2114)
        _log("擦除目标 Slot %s (DID 0x2114=%d)" % (slot_name(dest), dest))
        if dest not in (SLOT_A, SLOT_B):
            raise RuntimeError("DID 0x2114 槽号无效: %d" % dest)
        image = relocate_image_to_slot(image, dest, priv)
        size = len(image)
        addr_val = slot_base(dest)
        if DOWNLOAD_ADDR != addr_val:
            _log("0x34 地址用槽基址 0x%08X（配置 DOWNLOAD_ADDR=0x%08X 已忽略）" % (addr_val, DOWNLOAD_ADDR))
        sz = [(size >> 24) & 0xFF, (size >> 16) & 0xFF, (size >> 8) & 0xFF, size & 0xFF]
        addr = [(addr_val >> 24) & 0xFF, (addr_val >> 16) & 0xFF,
                (addr_val >> 8) & 0xFF, addr_val & 0xFF]
        _log("---- RequestDownload %d @ 0x%08X ----" % (size, addr_val))
        uds_req(bus_id, SID_RD, [0x00, 0x44] + addr + sz)
        _log("---- TransferData ----")
        seq = 1
        off = 0
        while off < size:
            if stopTask:
                raise RuntimeError("用户停止")
            chunk = image[off:off + TRANSFER_BLOCK_DATA]
            uds_req(bus_id, SID_TD, [seq] + _to_list(chunk))
            off += len(chunk)
            seq = 1 if seq == 0xFF else seq + 1
            if off == size or (off % (TRANSFER_BLOCK_DATA * 16) == 0):
                _log("  %d/%d" % (off, size))
        _log("---- TransferExit ----")
        last_err = None
        for attempt in range(1, 6):
            try:
                uds_req(bus_id, SID_RTE, [], wait_pending_s=45)
                last_err = None
                break
            except Exception as e:
                last_err = e
                _log("0x37 第 %d/5 次: %s" % (attempt, e))
                if attempt < 5:
                    time.sleep(0.8)
        if last_err is not None:
            raise last_err
        _log("---- Reset ----")
        uds_ecu_reset(bus_id)
        t0 = time.time()
        while time.time() - t0 < 2.0:
            if stopTask:
                raise RuntimeError("用户停止脚本")
            time.sleep(0.05)
        confirm_app_after_reset(bus_id)
        _log("======== OTA 成功 ========")
    finally:
        zcanpro.uds_deinit()


def z_main():
    global stopTask
    stopTask = False
    _log("======== Qi CAN-UDS OTA (APP 写对面槽) ========")
    buses = zcanpro.get_buses()
    _log("总线 " + str(buses))
    if not buses:
        _log("请先打开 CAN 通道 250kbps 扩展帧")
        return
    try:
        run_ota(buses[0]["busID"])
    except Exception as e:
        _log("OTA 失败: " + str(e))
