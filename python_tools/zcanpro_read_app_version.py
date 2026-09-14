# -*- coding: utf-8 -*-
"""
ZCANPRO 脚本 — 读取 APP 侧版本号 DID 0xF195 / 0xF180 / 0xF193

流程：
  1. TesterPresent 0x3E 00 连发两次（第一帧可能只唤醒 SIT1145 Standby）
  2. 原始 CAN 发 03 22 F1xx，收 ISO-TP 多帧并按 SN 组包
  3. 失败再试 zcanpro.uds_request

receive() 实际返回 (status, [frames])，不是帧字典列表。
"""

import time

try:
    import zcanpro
except Exception:
    zcanpro = None

UDS_REQ_ID  = 0x18DA0D03
UDS_RESP_ID = 0x18DA030D

SID_RDBI = 0x22
SID_TP   = 0x3E
SID_NRC  = 0x7F
SID_PR   = 0x40

DID_LIST = [
    (0xF195, "APP 软件版本"),
    (0xF180, "Bootloader 版本"),
    (0xF193, "硬件版本"),
]

stopTask = False
_tx_mode = None
_rx_logged = 0


def z_notify(type, obj):
    global stopTask
    if type == "stop":
        stopTask = True


def _log(msg):
    if zcanpro is not None:
        try:
            zcanpro.write_log(str(msg))
        except Exception:
            pass


def _hex(data):
    if data is None:
        return ""
    return " ".join("%02X" % (int(b) & 0xFF) for b in data)


def _pad8(data):
    d = [int(x) & 0xFF for x in list(data)]
    while len(d) < 8:
        d.append(0xCC)
    return d[:8]


def _uds_init():
    zcanpro.uds_init({
        "response_timeout_ms": 2000, "use_canfd": 0, "canfd_brs": 0,
        "trans_ver": 0, "fill_byte": 0xCC, "frame_type": 1,
        "trans_stmin_valid": 1, "trans_stmin": 10, "enhanced_timeout_ms": 8000,
    })


def _uds_deinit():
    try:
        zcanpro.uds_deinit()
    except Exception:
        pass


def _make_frame(can_id, data):
    # ZLG：bit31=1 表示扩展帧。上次 raw 发出的是 18da0d03（无 x）标准帧，MCU 滤掉。
    cid29 = int(can_id) & 0x1FFFFFFF
    cid = cid29 | 0x80000000
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
        "data": _pad8(data),
    }


def can_send(bus_id, can_id, data):
    global _tx_mode
    frame = _make_frame(can_id, data)
    attempts = [
        ("transmit(list)", "transmit", (bus_id, [frame])),
        ("transmit(dict)", "transmit", (bus_id, frame)),
        ("send(list)", "send", (bus_id, [frame])),
        ("send(dict)", "send", (bus_id, frame)),
    ]
    if _tx_mode:
        attempts = [a for a in attempts if a[0] == _tx_mode] + attempts
    last = None
    for label, name, args in attempts:
        fn = getattr(zcanpro, name, None)
        if fn is None:
            continue
        try:
            fn(*args)
            if _tx_mode != label:
                _tx_mode = label
                _log("CAN TX 使用 " + label)
            return
        except Exception as e:
            last = e
    raise RuntimeError("无法发送 CAN: %s" % last)


def _as_int(x):
    try:
        return int(x)
    except Exception:
        return None


def _parse_one_frame(f):
    """dict / list / tuple / object → (can_id, data_list) or None."""
    if f is None:
        return None
    if isinstance(f, dict):
        cid = None
        for k in ("can_id", "id", "CANID", "canid"):
            if k in f:
                cid = _as_int(f[k])
                break
        dat = f.get("data")
        if cid is None:
            return None
        return (cid & 0x1FFFFFFF, [int(x) & 0xFF for x in list(dat or [])])
    if isinstance(f, (list, tuple)):
        if len(f) >= 2 and _as_int(f[0]) is not None:
            cid = _as_int(f[0]) & 0x1FFFFFFF
            dat = f[1]
            if isinstance(dat, (list, tuple, bytes, bytearray)):
                return (cid, [int(x) & 0xFF for x in list(dat)])
        return None
    cid = _as_int(getattr(f, "can_id", getattr(f, "id", None)))
    dat = getattr(f, "data", None)
    if cid is None:
        return None
    return (cid & 0x1FFFFFFF, [int(x) & 0xFF for x in list(dat or [])])


def _unwrap_receive(raw):
    """ZCANPRO receive 常见返回：(status, [frames]) 或 [frames] 或 单帧。"""
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


def can_recv(bus_id):
    global _rx_logged
    raw = None
    try:
        raw = zcanpro.receive(bus_id)
    except TypeError:
        try:
            raw = zcanpro.receive()
        except Exception as e:
            _log("receive() 失败: " + str(e))
            return []
    except Exception as e:
        _log("receive(bus_id) 失败: " + str(e))
        return []

    if _rx_logged < 3:
        _rx_logged += 1
        _log("receive 原始: %s" % (repr(raw)[:240],))

    items = _unwrap_receive(raw)
    out = []
    for f in items:
        parsed = _parse_one_frame(f)
        if parsed is None:
            continue
        out.append(parsed)
    return out


def _assemble(head, total, cfs):
    buf = list(head)
    sn = 1
    while len(buf) < total:
        if sn not in cfs:
            return None
        buf.extend(cfs[sn])
        sn = (sn + 1) & 0x0F
    return buf[:total]


def isotp_raw_request(bus_id, sid, payload, timeout_s=2.5):
    req = [sid] + list(payload)
    pci = [len(req)] + req
    can_send(bus_id, UDS_REQ_ID, pci)
    _log("[Tx raw] %s" % _hex(_pad8(pci)))

    t0 = time.time()
    total = None
    head = None
    cfs = {}
    fc_sent = False
    saw = 0

    while (time.time() - t0) < timeout_s:
        if stopTask:
            raise RuntimeError("用户停止")
        for cid, dat in can_recv(bus_id):
            saw += 1
            if saw <= 12:
                _log("[raw RX] id=0x%08X %s" % (cid, _hex(dat[:8])))
            if cid != (UDS_RESP_ID & 0x1FFFFFFF):
                continue
            if not dat:
                continue
            pci_t = dat[0] & 0xF0
            if pci_t == 0x00:
                n = dat[0] & 0x0F
                return dat[1:1 + n]
            if pci_t == 0x10:
                total = ((dat[0] & 0x0F) << 8) | dat[1]
                head = list(dat[2:8])
                if not fc_sent:
                    can_send(bus_id, UDS_REQ_ID, [0x30, 0x00, 0x0A])
                    fc_sent = True
                    _log("[Tx raw] FC 30 00 0A")
                t0 = time.time()
            elif pci_t == 0x20:
                cfs[dat[0] & 0x0F] = list(dat[1:8])
                t0 = time.time()
            got = _assemble(head, total, cfs) if (head is not None and total is not None) else None
            if got is not None:
                return got
        time.sleep(0.01)
    raise RuntimeError("ISO-TP 组帧超时 (raw RX=%d 帧, CF SN=%s)" % (
        saw, ",".join("%d" % k for k in sorted(cfs.keys())) or "无"))


def uds_req(bus_id, sid, payload, timeout_note=""):
    req = {
        "src_addr": UDS_REQ_ID, "dst_addr": UDS_RESP_ID,
        "suppress_response": 0, "sid": sid, "data": list(payload),
    }
    _log("[Tx uds] %02X %s" % (sid, _hex(payload)))
    resp = zcanpro.uds_request(bus_id, req)
    data = list((resp or {}).get("data") or [])
    if data:
        _log("[Rx uds] %s" % _hex(data[:40]))
    if not resp or not resp.get("result"):
        raise RuntimeError("uds 无应答%s: %s" % (timeout_note, (resp or {}).get("result_msg", "")))
    return data


def parse_did_string(did, rx):
    if len(rx) >= 3 and rx[0] == SID_NRC:
        raise RuntimeError("NRC 0x%02X" % rx[2])
    if len(rx) < 1 or rx[0] != (SID_RDBI + SID_PR):
        raise RuntimeError("非正响应: %s" % _hex(rx))
    if len(rx) < 35:
        raise RuntimeError("DID 0x%04X 过短 %dB: %s" % (did, len(rx), _hex(rx)))
    raw = rx[3:35]
    return "".join(chr(b) if 0x20 <= b < 0x7F else "?" for b in raw).rstrip()


def wake_mcu(bus_id):
    """3E 00 是单帧正响应，可唤醒 Standby。第一帧可能被当 WUP 吃掉。"""
    _uds_init()
    ok = False
    for i in range(1, 4):
        if stopTask:
            raise RuntimeError("用户停止")
        try:
            rx = uds_req(bus_id, SID_TP, [0x00], timeout_note=" (唤醒第%d次)" % i)
            if rx and rx[0] == (SID_TP + SID_PR):
                _log("MCU 已在线 (7E)")
                ok = True
                break
        except Exception as e:
            _log("唤醒 %d/3: %s" % (i, e))
            time.sleep(0.15)
    return ok


def read_did_string(bus_id, did):
    payload = [(did >> 8) & 0xFF, did & 0xFF]
    last = []
    # 先原始扩展帧组包：uds_request 会自己发 FC STmin=0，CF 乱序后直接超时，
    # 且把 MCU 那一轮多帧吃掉，后面 raw 再发已晚。
    try:
        rx = isotp_raw_request(bus_id, SID_RDBI, payload)
        return parse_did_string(did, rx)
    except Exception as e:
        last.append("raw=" + str(e))
        _log("原始组帧失败，改试 uds_request: " + str(e))
    _uds_init()
    try:
        rx = uds_req(bus_id, SID_RDBI, payload)
        return parse_did_string(did, rx)
    except Exception as e:
        last.append("uds=" + str(e))
        raise RuntimeError(" ; ".join(last))
    finally:
        _uds_deinit()


def run(bus_id):
    _log("======== 读取 APP 侧版本号 ========")
    _log("CAN ID: Tx 0x%08X  Rx 0x%08X" % (UDS_REQ_ID, UDS_RESP_ID))
    names = [a for a in dir(zcanpro) if not a.startswith("_")]
    _log("zcanpro API: " + ", ".join(names))
    _log("")

    wake_mcu(bus_id)
    time.sleep(0.05)
    _uds_deinit()
    _log("UDS 已释放，改原始扩展帧读 DID（CAN 视图应变为 18da0d03x）")

    results = []
    for did, name in DID_LIST:
        try:
            ver = read_did_string(bus_id, did)
            _log("DID 0x%04X [%s]: %s" % (did, name, ver))
            results.append((name, ver, None))
        except Exception as e:
            _log("DID 0x%04X [%s]: 读取失败 - %s" % (did, name, e))
            results.append((name, None, str(e)))
        time.sleep(0.05)

    _log("")
    _log("---- 汇总 ----")
    for name, ver, err in results:
        if ver is not None:
            _log("  %s = %s" % (name, ver))
        else:
            _log("  %s = [失败] %s" % (name, err))


def z_main():
    global stopTask
    stopTask = False
    _log("======== APP 版本读取工具 ========")
    _log("DID: 0xF195(SW) / 0xF180(BL) / 0xF193(HW)")
    _log("")
    buses = zcanpro.get_buses()
    if not buses:
        _log("请先打开 CAN 通道 (250kbps, 扩展帧)")
        return
    _log("bus = " + str(buses[0]))
    try:
        run(buses[0]["busID"])
    except Exception as e:
        _log("失败: " + str(e))
    finally:
        _uds_deinit()
