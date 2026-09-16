# -*- coding: utf-8 -*-
"""
ZCANPRO 扩展脚本 — 从 Boot 升级 (自动识别 A/B 槽)

强制入口 Boot：跳过 APP 的 11 01，直接 10 02 → 27 → 31 → 34/36。
MCU 必须已在 Boot Safe Mode（空片、下载中、或刚 11 01 进来）。
若正在跑 APP，请用 from_app 脚本。

导入: 高级功能 -> 扩展脚本 -> 打开本文件
运行前: 先打开 CAN 通道 (250 kbps, Classical CAN, 扩展帧)
需要: Python 3.8 32 位（ZCANPRO 扩展脚本要求）
固件: 将 A/B 槽的 bin 放到 app bin/ 目录，脚本自动识别
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
# 自动识别 A/B 槽：读取固件 Reset Handler 地址，自动选择对应槽。
_TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(_TOOLS_DIR)
FIRMWARE_DIR = os.path.join(_TOOLS_DIR, "app bin")
# 自动识别模式下只认已打包的 bin（app_slot_a.bin / app_slot_b.bin）
PRIVATE_KEY_PATH = os.path.join(REPO_ROOT, "docs", "keys", "private.pem")
# auto=探测 APP/Boot；app=必须从 APP 11 01 进 Boot；boot=已在 Safe Mode，直接下载
ENTRY_MODE = "boot"

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


def _auto_select_firmware(bus_id):
    """读 MCU 当前活跃槽(DID 0x2113)，选对面槽的 bin。"""
    slots = _scan_firmware()
    if not slots:
        raise RuntimeError("app bin/ 中没有匹配 Slot A/B 的固件")
    active = read_did_u8(bus_id, 0x2113)
    _log("MCU 当前活跃槽: Slot %s" % slot_name(active))
    target = SLOT_B if active == SLOT_A else SLOT_A
    if target not in slots:
        raise RuntimeError("app bin/ 中缺少 Slot %s 的固件" % slot_name(target))
    _log("自动选择: %s → 升级 Slot %s" % (os.path.basename(slots[target]), slot_name(target)))
    return slots[target]

FIRMWARE_PATH = ""
DOWNLOAD_ADDR = 0x08007000
TRANSFER_BLOCK_DATA = 128

UDS_REQ_ID = 0x18DA0D03
UDS_RESP_ID = 0x18DA030D
SID_DSC, SID_ER, SID_RDBI, SID_SA = 0x10, 0x11, 0x22, 0x27
SID_WDBI, SID_RC, SID_RD, SID_TD, SID_RTE = 0x2E, 0x31, 0x34, 0x36, 0x37
SID_NRC, SID_PR = 0x7F, 0x40
NRC_SNS = 0x11
NRC_RCRRP = 0x78
SA_SIG_CHUNK = 4  # 27 03 单帧：SID+03+seq+4B = 7，避开 ISO-TP 多帧
IMAGE_MAGIC = 0x4F544158
IMAGE_HEADER_SIZE = 256
SLOT_A, SLOT_B = 0, 1
SLOT_A_BASE = 0x08007000
SLOT_B_BASE = 0x08011800
SLOT_SIZE = 0xA800
MAX_TD_DATA = 254

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
        raise RuntimeError("Reset Handler 0x%08X 不在 Slot A/B 内，请改 Target IROM1（A=0x08007100 / B=0x08011900）" % reset)
    _log("镜像链接 Slot %s, 总长 %d" % (slot_name(linked), len(image)))
    return linked


def pack_image_if_needed(fw_path, priv, version="1.0.0"):
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
    ver_s = version if version is not None else "1.0.0"
    if sys.version_info[0] >= 3:
        ver = (ver_s.encode("ascii", "replace") + b"\x00" * 16)[:16]
    else:
        ver = (str(ver_s) + ("\x00" * 16))[:16]
    header = struct.pack("<III", IMAGE_MAGIC, len(data), crc) + sig + ver + struct.pack("<I", int(time.time()) & 0xFFFFFFFF)
    header += b"\x00" * (IMAGE_HEADER_SIZE - len(header))
    _log("打包完成 crc=0x%08X version=%s" % (crc, ver_s))
    return header + data


def uds_init():
    zcanpro.uds_init({
        "response_timeout_ms": 3000,
        "use_canfd": 0,
        "canfd_brs": 0,
        "trans_ver": 0,
        "fill_byte": 0xCC,
        "frame_type": 1,
        "trans_stmin_valid": 1,
        "trans_stmin": 1,
        "enhanced_timeout_ms": 30000,
    })
    _log("UDS 就绪 0x18DA0D03 / 0x18DA030D 扩展帧")


def uds_req(bus_id, sid, payload, suppress=0, wait_pending_s=0):
    """NRC 0x78 is not final. For 0x27 02 / 0x37 pass wait_pending_s>0 to refresh."""
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


def send_security_key(bus_id, sig):
    """Send 64-byte P1363 signature as 16 single-frame 0x27 0x03 chunks, then 0x27 0x02.

    0x27 02 runs ECDSA P-256 on MCU (seconds). MCU first replies 7F 27 78.
    """
    sig = _to_bytes(sig)
    if len(sig) != 64:
        raise RuntimeError("ECDSA 签名须 64 字节, 实际 %d" % len(sig))
    seq = 1
    off = 0
    while off < 64:
        piece = sig[off:off + SA_SIG_CHUNK]
        uds_req(bus_id, SID_SA, [0x03, seq] + _to_list(piece))
        off += len(piece)
        seq += 1
    _log("27 03 已送 64 字节 / %d 帧" % (seq - 1))
    time.sleep(0.15)
    last = None
    for i in range(5):
        if stopTask:
            raise RuntimeError("用户停止脚本")
        try:
            return uds_req(bus_id, SID_SA, [0x02], wait_pending_s=45)
        except UdsNrcError as e:
            if (e.nrc in (0x24, 0x13)) and (i > 0):
                rx = uds_try(bus_id, SID_SA, [0x01])
                if rx is not None and len(rx) >= 6 and list(rx[2:6]) == [0, 0, 0, 0]:
                    _log("27 02 无应答后已解锁，继续")
                    return rx
            raise
        except Exception as e:
            last = e
            _log("27 02 第 %d/5 次: %s" % (i + 1, e))
            time.sleep(0.5)
            rx = uds_try(bus_id, SID_SA, [0x01])
            if rx is not None and len(rx) >= 6 and list(rx[2:6]) == [0, 0, 0, 0]:
                _log("27 01 seed=0，已解锁")
                return rx
    raise last


def confirm_app_after_reset(bus_id):
    """Wait for MCU after 0x11. Boot re-verifies ECDSA before jump (several seconds).
    Do not use DID 0xF195: APP/Boot pad 32 bytes (ISO-TP FF)."""
    last_err = None
    rx = None
    t0 = time.time()
    _log("等待 APP 起来（Boot 跳转前还要验签，可能数秒）")
    while time.time() - t0 < 25.0:
        if stopTask:
            raise RuntimeError("用户停止脚本")
        try:
            rx = uds_req(bus_id, SID_RDBI, [0x21, 0x13])
            last_err = None
            break
        except Exception as e:
            last_err = e
            _log("复位后 22 2113 等待: " + str(e))
            time.sleep(0.5)
    if last_err is not None:
        raise RuntimeError("复位后无 UDS（跳转失败或 APP CAN 未起来）: " + str(last_err))
    _log("复位后 DID 0x2113 slot=" + _hex((rx or [])[3:4]))
    try:
        fw = uds_req(bus_id, SID_RDBI, [0x20, 0x10])
        _log("DID 0x2010 fw_type=" + _hex(fw[3:4]))
    except UdsNrcError as e:
        _log("DID 0x2010 NRC 0x%02X（Boot 无此 DID）" % e.nrc)
    try:
        uds_req(bus_id, SID_RD, [0x00])
    except UdsNrcError as e:
        if e.nrc == NRC_SNS:
            _log("复位后 0x34 NRC 0x11，已在 APP")
            return
        raise RuntimeError("复位后 0x34 异常: " + str(e))
    raise RuntimeError("复位后 0x34 未回 NRC 0x11，仍在 Bootloader")


def probe_in_app(bus_id):
    """APP 对 0x34 回 NRC 0x11；Boot 为 0x22/0x24/0x33 等。"""
    try:
        uds_req(bus_id, SID_RD, [0x00])
        _log("探测 0x34 正响应，视为已在 Boot")
        return False
    except UdsNrcError as e:
        if e.nrc == NRC_SNS:
            _log("探测 0x34 NRC 0x11，当前在 APP")
            return True
        _log("探测 0x34 NRC 0x%02X，视为已在 Boot" % e.nrc)
        return False


def enter_boot_from_app(bus_id, priv):
    _log("---- APP 进 Boot (10 02 → 27 → 11 01) ----")
    try:
        uds_req(bus_id, SID_DSC, [0x02])
        rx = uds_req(bus_id, SID_SA, [0x01])
        if len(rx) >= 34:
            seed = _to_bytes(rx[2:34])
            if seed == b"\x00" * 32:
                _log("APP 已解锁 (seed=0)")
            else:
                send_security_key(bus_id, ecdsa_sign_msg(priv, seed))
    except Exception as e:
        _log("APP 10 02/27 失败，仍尝试 11 01: " + str(e))
    time.sleep(0.05)
    uds_ecu_reset(bus_id)
    t0 = time.time()
    while time.time() - t0 < 1.0:
        if stopTask:
            raise RuntimeError("用户停止脚本")
        time.sleep(0.05)


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
        FIRMWARE_PATH = _auto_select_firmware(bus_id)
        if not os.path.isfile(FIRMWARE_PATH):
            raise RuntimeError("找不到固件: " + FIRMWARE_PATH)
        image = pack_image_if_needed(FIRMWARE_PATH, priv)
        linked = validate_image(image)
        mode = ENTRY_MODE
        if mode == "app":
            enter_boot_from_app(bus_id, priv)
        elif mode == "boot":
            _log("入口=Boot，跳过 APP 11 01，直接下载")
        else:
            if probe_in_app(bus_id):
                enter_boot_from_app(bus_id, priv)
            else:
                _log("入口=Boot（自动探测），直接下载")
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
        rx = uds_req(bus_id, SID_SA, [0x01])
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
            _log("SendKey 签名 %d 字节（27 03 分片 + 27 02 验签）" % len(sig))
            send_security_key(bus_id, sig)
        _log("---- DID 0x2010 APP ----")
        uds_req(bus_id, SID_WDBI, [0x20, 0x10, 0x01])
        _log("---- 擦除 ----")
        uds_req(bus_id, SID_RC, [0x01, 0xFF, 0x00], wait_pending_s=45)
        dest = read_did_u8(bus_id, 0x2114)
        _log("擦除目标 Slot %s (DID 0x2114=%d)" % (slot_name(dest), dest))
        if dest not in (SLOT_A, SLOT_B):
            raise RuntimeError("DID 0x2114 槽号无效: %d" % dest)
        if dest != linked:
            raise RuntimeError(
                "MCU 写入 Slot %s，但镜像按 Slot %s 链接。请把 Target IROM1 Start 改为 0x%08X 后重编"
                % (slot_name(dest), slot_name(linked),
                   SLOT_A_BASE + IMAGE_HEADER_SIZE if dest == SLOT_A else SLOT_B_BASE + IMAGE_HEADER_SIZE)
            )
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
    _log("======== Qi CAN-UDS OTA auto (入口=%s) ========" % ENTRY_MODE)
    _log("固件 " + FIRMWARE_PATH)
    buses = zcanpro.get_buses()
    _log("总线 " + str(buses))
    if not buses:
        _log("请先打开 CAN 通道 250kbps 扩展帧")
        return
    try:
        run_ota(buses[0]["busID"])
    except Exception as e:
        _log("OTA 失败: " + str(e))
