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
PROBE_ROUNDS = 3

# secp256r1 / prime256v1. n 必须与 bootloader uECC.c 的 N[] 一致。
_P = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF
_N = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
_A = _P - 3
_GX = 0x6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296
_GY = 0x4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5

stopTask = False
# 原始帧发送模式缓存（transmit/send × list/dict），首次命中后固定，
# 避免每次4种组合都试一遍。见 _can_send_raw。
_raw_tx_mode = None


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
    ver = image[76:92]
    ts = image[92:96]
    header = struct.pack("<III", IMAGE_MAGIC, len(payload), crc) + sig + ver + ts
    header += b"\x00" * (IMAGE_HEADER_SIZE - len(header))
    _log("镜像 Slot %s → Slot %s，重定位 %d 处地址 crc=0x%08X" % (
        slot_name(linked), slot_name(dest), n, crc))
    return header + payload



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
    """NRC 0x78 is not final.

    注意（实测定性）：本机 ZCANPRO 库内部消化 NRC 0x78（不把 7F xx 78 当返回
    数据抛给脚本），并把 enhanced_timeout_ms 当绝对上限计时。因此下面
    wait_pending_s>0 的 0x78 重试分支在本库上是死代码——长操作一律在
    enhanced_timeout_ms 整点被掐断。仅当换用会把 7F xx 78 当 data 返回、
    且每收 78 刷新计时的库时，该分支才生效。长耗时请求（如 0x31 擦除）请走
    uds_req_raw_longop（原始收发 + 自管 0x78 看门狗），不要依赖 wait_pending_s。
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
    """Send 64-byte P1363 signature.

    Prefer 27 02 + 64B in one ISO-TP message (APP copies data[2..65]).
    Some builds reject 27 03 with NRC 0x12; 27 03 is only a fallback.
    """
    sig = _to_bytes(sig)
    if len(sig) != 64:
        raise RuntimeError("ECDSA 签名须 64 字节, 实际 %d" % len(sig))
    try:
        _log("SendKey 27 02 + 64 字节")
        return uds_req(bus_id, SID_SA, [0x02] + _to_list(sig), wait_pending_s=45)
    except UdsNrcError as e:
        if e.nrc not in (0x12, 0x13, 0x24):
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
        _log("复位后 0x34 正响应，已在 APP")
    except UdsNrcError as e:
        _log("复位后 0x34 NRC 0x%02X，已在 APP（默认会话下正常）" % e.nrc)
    except RuntimeError as e:
        # Boot 不应答任何 UDS：无应答只说明链路未通，不能反推运行位置
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


def _recv_frames(bus_id):
    """ZCANPRO 原始收帧。实测 receive(bus_id) 返回 (status, [frames])，
    个别版本无参；任何异常都返回空表，由调用方按超时处理。"""
    try:
        raw = zcanpro.receive(bus_id)
    except TypeError:
        try:
            raw = zcanpro.receive()
        except Exception:
            return []
    except Exception:
        return []
    if raw is None:
        return []
    if isinstance(raw, tuple) or (isinstance(raw, list) and len(raw) == 2
                                  and not isinstance(raw[0], dict)
                                  and isinstance(raw[1], (list, tuple))):
        a, b = raw[0], raw[1]
        items = list(b) if isinstance(b, (list, tuple)) else (list(a) if isinstance(a, (list, tuple)) else [])
    elif isinstance(raw, dict):
        items = [raw]
    elif isinstance(raw, (list, tuple)):
        items = list(raw)
    else:
        items = [raw]
    out = []
    for f in items:
        p = _parse_can_frame(f)
        if p is not None:
            out.append(p)
    return out


def _make_raw_frame(can_id, data):
    """构造 ZLG 扩展帧原始发送帧。与 zcanpro_read_app_version.py 实测一致：
    bit31=1 是 ZLG 扩展帧标志；is_extend 等多键名兼容不同 zcanpro 版本。"""
    cid29 = int(can_id) & 0x1FFFFFFF
    cid = cid29 | 0x80000000
    d = [int(x) & 0xFF for x in data]
    while len(d) < 8:
        d.append(0xCC)
    return {
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
        "data": d[:8],
    }


def _can_send_raw(bus_id, can_id, data):
    """原始扩展帧发送。依次尝试 transmit/send 的 list/dict 形式，
    命中后缓存模式（zcanpro_read_app_version.py 实测模式）。"""
    global _raw_tx_mode
    frame = _make_raw_frame(can_id, data)
    attempts = [
        ("transmit(list)", "transmit", (bus_id, [frame])),
        ("transmit(dict)", "transmit", (bus_id, frame)),
        ("send(list)", "send", (bus_id, [frame])),
        ("send(dict)", "send", (bus_id, frame)),
    ]
    if _raw_tx_mode:
        attempts = [a for a in attempts if a[0] == _raw_tx_mode] + attempts
    last = None
    for label, name, args in attempts:
        fn = getattr(zcanpro, name, None)
        if fn is None:
            continue
        try:
            fn(*args)
            if _raw_tx_mode != label:
                _raw_tx_mode = label
                _log("原始 TX 使用 " + label)
            return
        except Exception as e:
            last = e
    raise RuntimeError("原始帧发送失败: %s" % last)


def uds_req_raw_longop(bus_id, sid, payload, per_78_s=10.0, total_s=120.0, desc=""):
    """绕开 zcanpro.uds_request，用原始收发实现长耗时 UDS 请求，自管 0x78 看门狗。

    动机（实测定性）：ZCANPRO 库把 NRC 0x78 内部消化（不把 7F xx 78 当返回数据
    抛给脚本），并把 enhanced_timeout_ms 当绝对上限计时——因此 uds_req 里
    wait_pending_s 的 0x78 重试循环永不触发（死代码），长操作一律在
    enhanced_timeout_ms 整点被掐断（本次擦除失败正是整30s）。改原始收发后：
      - 每收到一帧 7F <sid> 78 刷新 per_78_s 看门狗（健康擦除时78每几ms~几十ms
        一帧，10s 极宽松；擦完 metadata 到正响应的静默也远小于10s）；
      - 见到正响应 <sid+0x40> 或最终 NRC 立即结束；
      - 总时长封顶 total_s；
      - 命中的 UDS_RESP_ID 原始帧全部打日志，下次失败可直接区分
        「MCU 没发78（真挂死）」还是「库吞了78」——这是旧路径给不出的诊断。

    时序：先 uds_deinit 释放通道（UDS 占用时 raw receive 不可靠，见 wake_bus
    与 zcanpro_read_app_version.py 实测），收发结束 finally 里 uds_init 恢复。
    deinit/init 只动测试端 ISO-TP 栈、不发帧给 ECU，会话/解锁状态保持。
    """
    if stopTask:
        raise RuntimeError("用户停止脚本")
    # ISO-TP 单帧：PCI=0x0N（N=1 SID + len(payload)），后跟 UDS 字节，不足补 0xCC
    sf = [len(payload) + 1, sid] + [int(x) & 0xFF for x in payload]
    tag = desc or ("SID=0x%02X" % sid)
    _log("[Tx][raw] %s: %02X %s" % (tag, sid, _hex(payload)))
    try:
        zcanpro.uds_deinit()
    except Exception as e:
        _log("UDS 通道释放失败（继续原始收发）: " + str(e))
    try:
        _can_send_raw(bus_id, UDS_REQ_ID, sf)
        t0 = time.time()
        t_total = t0 + float(total_s)
        t_78 = t0 + float(per_78_s)
        n78 = 0
        while True:
            if stopTask:
                raise RuntimeError("用户停止脚本")
            now = time.time()
            if now > t_total:
                raise RuntimeError("%s 超总上限 %.0fs（收到 %d 帧78），MCU 疑似挂死"
                                   % (tag, total_s, n78))
            if now > t_78:
                raise RuntimeError("%s 已 %.0fs 未见新78/最终响应（累计 %d 帧78），"
                                   "MCU 疑似擦除中挂死" % (tag, per_78_s, n78))
            for cid, dat in _recv_frames(bus_id):
                if (cid & 0x1FFFFFFF) != UDS_RESP_ID:
                    continue
                _log("[Rx][raw] %s" % _hex(dat[:8]))
                if not dat:
                    continue
                pci = dat[0]
                if (pci >> 4) != 0x0:  # 只处理单帧；长响应理论上不出现
                    _log("[Rx][raw] 非单帧 PCI=0x%02X，忽略" % pci)
                    continue
                ln = pci & 0x0F
                uds = [int(x) & 0xFF for x in dat[1:1 + ln]]
                if len(uds) >= 3 and uds[0] == SID_NRC and uds[1] == sid:
                    if uds[2] == NRC_RCRRP:
                        n78 += 1
                        t_78 = time.time() + float(per_78_s)  # 收到78即刷新看门狗
                        continue
                    raise UdsNrcError(sid, uds[2])
                if uds and uds[0] == (sid + SID_PR):
                    _log("%s 正响应，累计 %d 帧78" % (tag, n78))
                    return uds
            time.sleep(0.01)
    finally:
        uds_init()  # 恢复 UDS 通道，供后续 22/34/36/37 使用


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
    """Boot 无 UDS。APP 在线则 22 2113 有应答（正响应或 NRC）。

    不要用 0x34 探测：APP 已实现下载，默认会话回 0x22；且 Standby 下首帧
    只当 WUP，3s 超时后 ident 早已发出，再去听 0x18FF260D 会漏。
    无应答时连发 3E 80 再立刻重试 2113（MCU 此时应已 Normal）。"""
    for attempt in range(1, retries + 1):
        try:
            rx = uds_req(bus_id, SID_RDBI, [0x21, 0x13])
            _log("探测 22 2113 成功 slot=%s，当前在 APP" % _hex((rx or [])[3:4]))
            return True
        except UdsNrcError as e:
            _log("探测 22 2113 NRC 0x%02X，当前在 APP" % e.nrc)
            return True
        except RuntimeError as e:
            _log("探测 22 2113 无应答（第 %d/%d 轮）: %s" % (attempt, retries, e))
            if attempt < retries:
                for _ in range(3):
                    uds_try(bus_id, SID_TP, [0x80], suppress=1)
                    time.sleep(0.15)
                time.sleep(0.4)
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
            send_security_key(bus_id, sig)
        _log("---- DID 0x2010 APP ----")
        uds_req(bus_id, SID_WDBI, [0x20, 0x10, 0x01])
        _log("---- 擦除（原始收发 + 自管0x78看门狗，绕开库增强超时/吞0x78）----")
        uds_req_raw_longop(bus_id, SID_RC, [0x01, 0xFF, 0x00],
                           per_78_s=10.0, total_s=120.0, desc="31 01 FF 00 擦除")
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
