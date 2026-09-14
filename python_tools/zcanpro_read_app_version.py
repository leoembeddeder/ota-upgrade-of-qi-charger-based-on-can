# -*- coding: utf-8 -*-
"""
ZCANPRO 脚本 — 读取 APP 侧版本号

读取 DID 0xF195 / 0xF180 / 0xF193（各 32 字节 ASCII，ISO-TP 多帧）。
不需要编程会话或安全解锁。

ZCANPRO uds_request 对 35 字节正响应组帧不可靠（主机 FC 常为 STmin=0，
MCU 连续帧曾因 PTB/STB 乱序导致「无应答」）。本脚本用原始 CAN 自组 ISO-TP：
收到 First Frame 后回 FC（BS=0, STmin=10ms），按 SN 收集 Consecutive Frame。

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
    d = list(data)
    while len(d) < 8:
        d.append(0xCC)
    return d[:8]


def can_send(bus_id, can_id, data):
    msg = {
        "can_id": int(can_id) & 0x1FFFFFFF,
        "id": int(can_id) & 0x1FFFFFFF,
        "is_extend": 1,
        "is_extended": 1,
        "extend": 1,
        "frame_type": 1,
        "is_remote": 0,
        "is_canfd": 0,
        "data": _pad8(data),
        "data_len": 8,
        "len": 8,
    }
    last = None
    for name in ("transmit", "send", "send_can"):
        fn = getattr(zcanpro, name, None)
        if fn is None:
            continue
        try:
            fn(bus_id, msg)
            return
        except Exception as e:
            last = e
    raise RuntimeError("ZCANPRO 无法发送原始 CAN 帧: %s" % last)


def can_recv(bus_id):
    frames = None
    try:
        frames = zcanpro.receive(bus_id)
    except TypeError:
        try:
            frames = zcanpro.receive()
        except Exception:
            frames = None
    except Exception:
        frames = None
    if not frames:
        return []
    if isinstance(frames, dict):
        frames = [frames]
    out = []
    for f in frames:
        if not isinstance(f, dict):
            continue
        cid = f.get("can_id", f.get("id", f.get("CANID", 0)))
        dat = list(f.get("data") or [])
        try:
            cid = int(cid) & 0x1FFFFFFF
        except Exception:
            continue
        out.append((cid, dat))
    return out


def isotp_uds_request(bus_id, sid, payload, timeout_s=2.5):
    """SF 请求，组 FF+CF 响应。CF 按 SN 收集，允许乱序到达。"""
    req = [sid] + list(payload)
    if len(req) > 7:
        raise RuntimeError("本脚本只发单帧请求")
    pci = [len(req)] + req
    can_send(bus_id, UDS_REQ_ID, pci)
    _log("[Tx] %s" % _hex(pci))

    t0 = time.time()
    total = None
    head = None
    cfs = {}
    fc_sent = False

    while (time.time() - t0) < timeout_s:
        if stopTask:
            raise RuntimeError("用户停止")
        for cid, dat in can_recv(bus_id):
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
                    # BS=0 一次发完；STmin=10ms，减轻旧固件 mailbox 乱序
                    can_send(bus_id, UDS_REQ_ID, [0x30, 0x00, 0x0A])
                    fc_sent = True
                    _log("[Tx] FC 30 00 0A")
                t0 = time.time()
            elif pci_t == 0x20:
                sn = dat[0] & 0x0F
                cfs[sn] = list(dat[1:8])
                t0 = time.time()
        if head is not None and total is not None:
            buf = list(head)
            sn = 1
            while len(buf) < total:
                if sn not in cfs:
                    break
                buf.extend(cfs[sn])
                sn = (sn + 1) & 0x0F
            if len(buf) >= total:
                return buf[:total]
        time.sleep(0.01)
    raise RuntimeError("ISO-TP 组帧超时 (got CF SN=%s)" % (
        ",".join("%d" % k for k in sorted(cfs.keys())) or "无"))


def read_did_string(bus_id, did):
    payload = [(did >> 8) & 0xFF, did & 0xFF]
    rx = isotp_uds_request(bus_id, SID_RDBI, payload)
    _log("[Rx] %s" % _hex(rx[:40]))
    if len(rx) >= 3 and rx[0] == SID_NRC:
        raise RuntimeError("NRC 0x%02X" % rx[2])
    if len(rx) < 1 or rx[0] != (SID_RDBI + SID_PR):
        raise RuntimeError("非正响应: %s" % _hex(rx))
    if len(rx) < 35:
        raise RuntimeError("DID 0x%04X 响应过短 (%d 字节): %s" % (did, len(rx), _hex(rx)))
    raw = rx[3:35]
    return "".join(chr(b) if 0x20 <= b < 0x7F else "?" for b in raw).rstrip()


def run(bus_id):
    _log("======== 读取 APP 侧版本号 ========")
    _log("CAN ID: Tx 0x%08X  Rx 0x%08X" % (UDS_REQ_ID, UDS_RESP_ID))
    _log("ISO-TP: 原始组帧（不走 zcanpro.uds_request）")
    _log("")
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
        time.sleep(0.05)
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
    try:
        run(buses[0]["busID"])
    except Exception as e:
        _log("失败: " + str(e))
    finally:
        try:
            zcanpro.uds_deinit()
        except Exception:
            pass
