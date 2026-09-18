# -*- coding: utf-8 -*-
"""
ZCANPRO 脚本 — 设置 Qi 芯片功率

写 DID 0x210D（uint16 LE, 单位 mW）：
  0x01 = 5W (500mW), 0x02 = 10W (1000mW), 0x03 = 15W (1500mW)

需要：编程会话 + SecurityAccess

用法: ZCANPRO → 高级功能 → 扩展脚本 → 打开本文件
      修改下方 POWER_MW 值后运行
"""

import os
import sys
import time

try:
    import zcanpro
except ImportError:
    zcanpro = None

# ======== 用户配置 ========
def _find_repo_root(start):
    """向上探测仓库根目录（含 docs/keys 的祖先目录），不写死目录层级假设。"""
    d = os.path.abspath(start)
    while True:
        if os.path.isdir(os.path.join(d, "docs", "keys")):
            return d
        parent = os.path.dirname(d)
        if parent == d:
            return os.path.abspath(start)
        d = parent


_TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = _find_repo_root(_TOOLS_DIR)
PRIVATE_KEY_PATH = os.path.join(REPO_ROOT, "docs", "keys", "private.pem")

# 功率档位 (uint16 LE, mW)
POWER_MW = 1500  # 500=5W, 1000=10W, 1500=15W

# ======== UDS 常量 ========
UDS_REQ_ID  = 0x18DA0D03
UDS_RESP_ID = 0x18DA030D

SID_DSC  = 0x10
SID_SA   = 0x27
SID_WDBI = 0x2E
SID_NRC  = 0x7F
SID_PR   = 0x40

NRC_RCRRP = 0x78
NRC_EXCEEDED_ATTEMPTS   = 0x36  # 27 02 验签失败次数超限（fail_count≥3，固件随即锁定约30s）
NRC_REQUIRED_TIME_DELAY = 0x37  # 锁定期内 27 01/27 02 应答（requiredTimeDelay）


class UdsNrcError(RuntimeError):
    """带 NRC 码的 UDS 异常；SecurityAccess 重试分支按 e.nrc 判别设备锁定等场景。"""

    def __init__(self, sid, nrc):
        RuntimeError.__init__(self, "NRC SID=0x%02X NRC=0x%02X" % (sid, nrc))
        self.sid = sid
        self.nrc = nrc


DID_POWER_LIMIT = 0x210D
SA_SIG_CHUNK = 4

stopTask = False


def z_notify(type, obj):
    global stopTask
    if type == "stop":
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
    return list(b) if isinstance(b, (list, bytes)) else [ord(c) for c in b]


def _to_bytes(seq):
    if isinstance(seq, bytes):
        return seq
    return bytes(seq) if sys.version_info[0] >= 3 else "".join(chr(x & 0xFF) for x in seq)


# ======== ECDSA 签名 (P-256) ========
_P = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF
_N = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
_GX = 0x6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296
_GY = 0x4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5


def _inv(x, m):
    return pow(x % m, -1, m) if sys.version_info[0] >= 3 else pow(x % m, m - 2, m)


def _jp_double(x, y, z):
    if z == 0 or y == 0:
        return 0, 0, 0
    ysq = (y * y) % _P
    s = (4 * x * ysq) % _P
    m = (3 * x * x + (_P - 3) * pow(z, 4, _P)) % _P
    return (m * m - 2 * s) % _P, (m * (s - (m * m - 2 * s) % _P) - 8 * pow(ysq, 2, _P)) % _P, (2 * y * z) % _P


def _jp_add(x1, y1, z1, x2, y2, z2):
    if z1 == 0:
        return x2, y2, z2
    if z2 == 0:
        return x1, y1, z1
    z1z1, z2z2 = pow(z1, 2, _P), pow(z2, 2, _P)
    u1, u2 = x1 * z2z2 % _P, x2 * z1z1 % _P
    s1, s2 = y1 * z2 % _P * z2z2 % _P, y2 * z1 % _P * z1z1 % _P
    if u1 == u2:
        return _jp_double(x1, y1, z1) if (s1 + s2) % _P != 0 else (0, 0, 0)
    h = (u2 - u1) % _P
    r = (s2 - s1) % _P
    h2 = h * h % _P
    h3 = h * h2 % _P
    nx = (r * r - h3 - 2 * u1 * h2) % _P
    ny = (r * (u1 * h2 - nx) - s1 * h3) % _P
    return nx, ny, h * z1 % _P * z2 % _P


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
    z2 = zinv * zinv % _P
    return rx * z2 % _P, ry * z2 % _P * zinv % _P


def ecdsa_sign_msg(priv, msg):
    import hashlib
    h = hashlib.sha256(msg).digest()
    z = int.from_bytes(h, "big") % _N
    while True:
        k = int.from_bytes(os.urandom(32), "big") % _N
        if k == 0:
            continue
        x, _ = _jp_mul(k, _GX, _GY)
        r = x % _N
        if r == 0:
            continue
        s = _inv(k, _N) * (z + r * priv) % _N
        if s == 0:
            continue
        return r.to_bytes(32, "big") + s.to_bytes(32, "big")


def _collect_octet32(buf, out, start, end):
    i = start
    while i < end:
        tag = buf[i]
        i += 1
        if i >= end:
            break
        first = buf[i]
        i += 1
        ln = first if first < 0x80 else 0
        if first >= 0x80:
            n = first & 0x7F
            if 0 < n <= 4 and i + n <= end:
                ln = 0
                for _ in range(n):
                    ln = (ln << 8) | buf[i]
                    i += 1
            else:
                break
        if i + ln > end:
            break
        if tag == 0x04 and ln == 32:
            out.append(buf[i:i + 32])
        elif tag in (0x30, 0x31, 0xA0, 0xA1):
            _collect_octet32(buf, out, i, i + ln)
        i += ln


def load_ec_private_key(path):
    raw = open(path, "rb").read()
    if len(raw) == 32:
        priv = int.from_bytes(raw, "big")
        if 0 < priv < _N:
            return priv
    text = raw.decode("ascii", "ignore")
    if "BEGIN" in text:
        lines = [l.strip() for l in text.splitlines() if l.strip() and "BEGIN" not in l and "END" not in l]
        import base64
        der = base64.b64decode("".join(lines))
    else:
        der = raw
    cands = []
    _collect_octet32(der, cands, 0, len(der))
    for key in cands:
        if len(key) == 32:
            priv = int.from_bytes(key, "big")
            if 0 < priv < _N:
                return priv
    raise ValueError("无法解析私钥: " + path)


# ======== UDS 通信 ========

def uds_init():
    zcanpro.uds_init({
        "response_timeout_ms": 3000, "use_canfd": 0, "canfd_brs": 0,
        "trans_ver": 0, "fill_byte": 0xCC, "frame_type": 1,
        "trans_stmin_valid": 1, "trans_stmin": 1, "enhanced_timeout_ms": 30000,
    })


def uds_req(bus_id, sid, payload, suppress=0, wait_pending_s=0):
    if stopTask:
        raise RuntimeError("用户停止")
    req = {
        "src_addr": UDS_REQ_ID, "dst_addr": UDS_RESP_ID,
        "suppress_response": 1 if suppress else 0, "sid": sid, "data": list(payload),
    }
    t_end = time.time() + float(wait_pending_s)
    logged = False
    while True:
        if stopTask:
            raise RuntimeError("用户停止")
        if not logged:
            _log("[Tx] %02X %s" % (sid, _hex(payload[:20])))
            logged = True
        resp = zcanpro.uds_request(bus_id, req)
        if suppress:
            return None
        data = list((resp or {}).get("data") or [])
        if data:
            _log("[Rx] " + _hex(data[:24]))
        if len(data) >= 3 and data[0] == SID_NRC:
            if data[2] == NRC_RCRRP:
                if wait_pending_s <= 0 or time.time() >= t_end:
                    raise RuntimeError("NRC 0x78 超时")
                _log("NRC 0x78，等待中...")
                time.sleep(1.0)
                continue
            raise UdsNrcError(data[1], data[2])
        if not resp or not resp.get("result"):
            raise RuntimeError("无应答")
        if data[0] != (sid + SID_PR):
            raise RuntimeError("非正响应: " + _hex(data))
        return data


def _sa_fetch_seed(bus_id):
    """27 01 取 seed。固件自 d64e8c2 起 seed 为 32 字节（67 01 + 32B）。
    固件每次 27 01 都刷新 seed 并清空签名缓冲。"""
    rx = uds_req(bus_id, SID_SA, [0x01])
    if len(rx) < 34:
        raise RuntimeError("seed 响应过短: %d 字节, 期望 67 01 + 32 字节 seed（≥34）" % len(rx))
    return rx


def _sa_send_sig(bus_id, sig):
    """27 03 分片发送 64 字节签名：4 字节/帧 × 16 帧，blockSeq 0x01 起递增。"""
    sig = _to_list(sig)
    if len(sig) != 64:
        raise RuntimeError("ECDSA 签名须 64 字节")
    seq, off = 1, 0
    while off < 64:
        piece = sig[off:off + SA_SIG_CHUNK]
        uds_req(bus_id, SID_SA, [0x03, seq] + piece)
        off += len(piece)
        seq += 1
    _log("27 03 已送 64 字节 / %d 帧" % (seq - 1))
    time.sleep(0.15)


def send_security_key(bus_id, priv):
    """SecurityAccess 解锁：每次尝试都是完整流程——
    27 01 取 32 字节 seed（全 0 = 已解锁，直接返回正响应）→
    ecdsa_sign_msg(priv, seed) 重签 → 重发 16 帧 27 03 分片 → 27 02 验签。

    固件每次 27 01 都刷新 seed 并清空签名缓冲，27 02 失败后只重发 27 02
    或沿用旧 seed 签名必失败，故重试必须完整重做。最多 5 次完整尝试；
    NRC 0x36/0x37 = 设备 SecurityAccess 锁定（fail_count≥3，约 30s）：
    0x36 是 27 02 失败超限当次应答，0x37 是锁定期内 27 01/27 02 应答，
    两者都日志明确提示并 sleep 31s 后继续完整流程。
    """
    last = None
    for attempt in range(1, 6):
        if stopTask:
            raise RuntimeError("用户停止")
        try:
            _log("SecurityAccess 第 %d/5 次：27 01 → 重签 → 27 03 → 27 02" % attempt)
            rx = _sa_fetch_seed(bus_id)
            seed = _to_bytes(rx[2:34])
            if seed == b"\x00" * 32:
                _log("27 01 seed=0（32 字节全 0），已解锁，直接返回正响应")
                return rx
            _log("seed(32B) " + _hex(rx[2:34]))
            sig = ecdsa_sign_msg(priv, seed)
            _sa_send_sig(bus_id, sig)
            return uds_req(bus_id, SID_SA, [0x02], wait_pending_s=45)
        except UdsNrcError as e:
            last = e
            if e.nrc in (NRC_EXCEEDED_ATTEMPTS, NRC_REQUIRED_TIME_DELAY):
                _log("NRC 0x%02X：设备 SecurityAccess 锁定（fail_count≥3，固件锁定约30s），等待 31s 后完整重试" % e.nrc)
                time.sleep(31)
                continue
            _log("SecurityAccess 第 %d/5 次失败: %s（重试将重新取 seed 重签重发分片）" % (attempt, e))
            time.sleep(0.5)
        except RuntimeError as e:
            last = e
            _log("SecurityAccess 第 %d/5 次失败: %s" % (attempt, e))
            time.sleep(0.5)
    raise last


# ======== 主流程 ========

def run(bus_id):
    global stopTask
    power_name = {500: "5W", 1000: "10W", 1500: "15W"}
    if POWER_MW not in power_name:
        raise RuntimeError("无效功率: %d mW (可选 500/1000/1500)" % POWER_MW)

    _log("======== 设置 Qi 功率: %s ========" % power_name[POWER_MW])

    if not os.path.isfile(PRIVATE_KEY_PATH):
        raise RuntimeError("找不到私钥: " + PRIVATE_KEY_PATH)
    priv = load_ec_private_key(PRIVATE_KEY_PATH)

    uds_init()

    # 1. 编程会话
    _log("---- 进入编程会话 ----")
    uds_req(bus_id, SID_DSC, [0x02])

    # 2. 安全解锁（seed 32 字节，d64e8c2 起；失败重试为完整重签重发流程）
    _log("---- 安全解锁 ----")
    unlocked = False
    try:
        rx = uds_req(bus_id, SID_SA, [0x01])
        if len(rx) < 34:
            raise RuntimeError("seed 响应过短: %d 字节, 期望 67 01 + 32 字节 seed（≥34）" % len(rx))
        seed = _to_bytes(rx[2:34])
        _log("seed(32B) " + _hex(rx[2:34]))
        if seed == b"\x00" * 32:
            unlocked = True
            _log("已解锁 (seed=0，32 字节全 0)")
    except UdsNrcError as e:
        if e.nrc not in (NRC_EXCEEDED_ATTEMPTS, NRC_REQUIRED_TIME_DELAY):
            raise
        _log("27 01 NRC 0x%02X：设备 SecurityAccess 锁定（fail_count≥3，约30s），等待 31s 后完整解锁" % e.nrc)
        time.sleep(31)
    if not unlocked:
        _log("SecurityAccess 解锁中（每次尝试完整重做：27 01 → 重签 → 27 03 → 27 02）...")
        send_security_key(bus_id, priv)
    _log("安全解锁成功")

    # 3. 写功率 (DID 0x210D, uint16 LE mW)
    _log("---- 写入功率 %d mW ----" % POWER_MW)
    uds_req(bus_id, SID_WDBI, [0x21, 0x0D, POWER_MW & 0xFF, (POWER_MW >> 8) & 0xFF])
    _log("功率已设置: %s" % power_name[POWER_MW])


def z_main():
    global stopTask
    stopTask = False
    _log("======== Qi 功率设置工具 ========")
    _log("目标功率: %d mW" % POWER_MW)
    buses = zcanpro.get_buses()
    if not buses:
        _log("请先打开 CAN 通道 (250kbps, 扩展帧)")
        return
    try:
        run(buses[0]["busID"])
    except Exception as e:
        _log("失败: " + str(e))
    finally:
        try:
            zcanpro.uds_deinit()
        except Exception:
            pass
