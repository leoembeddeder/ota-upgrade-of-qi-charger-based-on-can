# -*- coding: utf-8 -*-
"""
ZCANPRO 脚本 — 读取 APP 侧版本号

读取 DID 0xF195 / 0xF180 / 0xF193（各 32 字节 ASCII，ISO-TP 多帧）。

ZCANPRO 官方 transmit 接收的是「帧列表」，不是单个 dict。
收到 First Frame 后回 FC 30 00 0A，按 SN 收集 Consecutive Frame（允许乱序）。
uds_request 作为兜底（MCU 已修 CF 乱序后可用）。

用法: ZCANPRO → 高级功能 → 扩展脚本 → 打开本文件
运行前: 打开 CAN 通道 250 kbps、Classical CAN、扩展帧
"""

import time

try:
    import zcanpro
except Exception:
    zcanpro = None

UDS_REQ_ID  = 0x18DA0D03
UDS_RESP_ID = 0x18DA030D

SID_RDBI = 0x22
SID_NRC  = 0x7F
SID_PR   = 0x40

DID_SW_VERSION         = 0xF195
DID_BOOTLOADER_VERSION = 0xF180
DID_HW_VERSION         = 0xF193

DID_LIST = [
    (DID_SW_VERSION,         "APP 软件版本"),
    (DID_BOOTLOADER_VERSION, "Bootloader 版本"),
    (DID_HW_VERSION,         "硬件版本"),
]

stopTask = False
_tx_mode = None
_api_logged = False


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
    d = [int(x) & 0xFF for x in data]
    while len(d) < 8:
        d.append(0xCC)
    return d[:8]


def _log_api_once():
    global _api_logged
    if _api_logged:
        return
    _api_logged = True
    names = [a for a in dir(zcanpro) if not a.startswith("_")]
    _log("zcanpro API: " + ", ".join(names))


def _make_frame(can_id, data):
    return {
        "can_id": int(can_id) & 0x1FFFFFFF,
        "is_canfd": 0,
        "canfd_brs": 0,
        "is_extend": 1,
        "frame_type": 1,
        "data": _pad8(data),
    }


def can_send(bus_id, can_id, data):
    """ZCANPRO demo: transmit(bus_id, [ {can_id, is_canfd, canfd_brs, data} ])."""
    global _tx_mode
    frame = _make_frame(can_id, data)
    attempts = [
        ("transmit(list)", "transmit", (bus_id, [frame])),
        ("transmit(dict)", "transmit", (bus_id, frame)),
        ("send(list)", "send", (bus_id, [frame])),
        ("send(dict)", "send", (bus_id, frame)),
    ]
    if _tx_mode is not None:
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
            continue
    raise RuntimeError("ZCANPRO 无法发送原始 CAN 帧: %s" % last)


def _frame_id(f):
    if not isinstance(f, dict):
        return None
    for k in ("can_id", "id", "CANID", "canid"):
        if k in f and f[k] is not None:
            try:
                return int(f[k]) & 0x1FFFFFFF
            except Exception:
                pass
    return None


def _frame_data(f):
    if not isinstance(f, dict):
        return []
    dat = f.get("data")
    if dat is None:
        return []
    try:
        return [int(x) & 0xFF for x in list(dat)]
    except Exception:
        return []


def can_recv(bus_id):
    frames = None
    try:
        frames = zcanpro.receive(bus_id)
    except TypeError:
        try:
            frames = zcanpro.receive()
        except Exception as e:
            _log("receive() 失败: " + str(e))
            return []
    except Exception as e:
        _log("receive(bus_id) 失败: " + str(e))
        return []
    if not frames:
        return []
    if isinstance(frames, dict):
        frames = [frames]
    out = []
    for f in frames:
        cid = _frame_id(f)
        if cid is None:
            _log("忽略未知帧: %s" % str(f)[:160])
            continue
        out.append((cid, _frame_data(f), f))
    return out


def _assemble_cfs(head, total, cfs):
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
    if len(req) > 7:
        raise RuntimeError("本脚本只发单帧请求")
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
        for cid, dat, raw in can_recv(bus_id):
            saw += 1
            if saw <= 8:
                _log("[raw RX] id=0x%08X %s" % (cid, _hex(dat[:8])))
            if cid != (UDS_RESP_ID & 0x1FFFFFFF):
                continue
            if len(dat) < 1:
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
            got = _assemble_cfs(head, total, cfs) if (head is not None and total is not None) else None
            if got is not None:
                return got
        time.sleep(0.01)

    raise RuntimeError("ISO-TP 组帧超时 (raw RX=%d 帧, CF SN=%s)" % (
        saw, ",".join("%d" % k for k in sorted(cfs.keys())) or "无"))


def uds_stack_request(bus_id, sid, payload):
    zcanpro.uds_init({
        "response_timeout_ms": 3000, "use_canfd": 0, "canfd_brs": 0,
        "trans_ver": 0, "fill_byte": 0xCC, "frame_type": 1,
        "trans_stmin_valid": 1, "trans_stmin": 10, "enhanced_timeout_ms": 30000,
    })
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
        raise RuntimeError("uds_request 无应答: %s" % (resp or {}).get("result_msg", ""))
    return data


def parse_did_string(did, rx):
    if len(rx) >= 3 and rx[0] == SID_NRC:
        raise RuntimeError("NRC 0x%02X" % rx[2])
    if len(rx) < 1 or rx[0] != (SID_RDBI + SID_PR):
        raise RuntimeError("非正响应: %s" % _hex(rx))
    if len(rx) < 35:
        raise RuntimeError("DID 0x%04X 响应过短 (%d 字节): %s" % (did, len(rx), _hex(rx)))
    raw = rx[3:35]
    return "".join(chr(b) if 0x20 <= b < 0x7F else "?" for b in raw).rstrip()


def read_did_string(bus_id, did):
    payload = [(did >> 8) & 0xFF, did & 0xFF]
    last = None
    try:
        rx = isotp_raw_request(bus_id, SID_RDBI, payload)
        return parse_did_string(did, rx)
    except Exception as e:
        last = e
        _log("原始组帧失败，改试 uds_request: " + str(e))
    try:
        rx = uds_stack_request(bus_id, SID_RDBI, payload)
        return parse_did_string(did, rx)
    except Exception as e:
        raise RuntimeError("raw=%s ; uds=%s" % (last, e))
    finally:
        try:
            zcanpro.uds_deinit()
        except Exception:
            pass


def run(bus_id):
    _log("======== 读取 APP 侧版本号 ========")
    _log("CAN ID: Tx 0x%08X  Rx 0x%08X" % (UDS_REQ_ID, UDS_RESP_ID))
    _log("")
    _log_api_once()
    try:
        zcanpro.uds_deinit()
    except Exception:
        pass
    results = []
    for did, name in DID_LIST:
        try:
            ver_str = read_did_string(bus_id, did)
            _log("DID 0x%04X [%s]: %s" % (did, name, ver_str))
            results.append((did, name, ver_str, None))
        except Exception as e:
            _log("DID 0x%04X [%s]: 读取失败 - %s" % (did, name, str(e)))
            results.append((did, name, None, str(e)))
        time.sleep(0.1)
        try:
            zcanpro.uds_deinit()
        except Exception:
            pass
    _log("")
    _log("---- 汇总 ----")
    for did, name, ver_str, err in results:
        if ver_str is not None:
            _log("  %s = %s" % (name, ver_str))
        else:
            _log("  %s = [失败] %s" % (name, err))
    return results


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
        try:
            zcanpro.uds_deinit()
        except Exception:
            pass
