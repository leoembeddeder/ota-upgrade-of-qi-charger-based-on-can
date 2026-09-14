# -*- coding: utf-8 -*-
"""
ZCANPRO 脚本 — 读取 APP 侧版本号

读取 DID 0xF195 / 0xF180 / 0xF193，返回 32 字节 ASCII 版本字符串。
不需要编程会话或安全解锁，任意会话可读。

用法: ZCANPRO → 高级功能 → 扩展脚本 → 打开本文件
"""

import sys
import time

try:
    import zcanpro
except ImportError:
    zcanpro = None

# ======== UDS 常量 ========
UDS_REQ_ID  = 0x18DA0D03
UDS_RESP_ID = 0x18DA030D

SID_RDBI = 0x22
SID_NRC  = 0x7F
SID_PR   = 0x40

NRC_RCRRP = 0x78

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


# ======== UDS 通信 ========

def uds_init():
    zcanpro.uds_init({
        "response_timeout_ms": 5000, "use_canfd": 0, "canfd_brs": 0,
        "trans_ver": 0, "fill_byte": 0xCC, "frame_type": 1,
        "trans_stmin_valid": 1, "trans_stmin": 1, "enhanced_timeout_ms": 30000,
    })


def uds_req(bus_id, sid, payload, wait_pending_s=0):
    if stopTask:
        raise RuntimeError("用户停止")
    req = {
        "src_addr": UDS_REQ_ID, "dst_addr": UDS_RESP_ID,
        "suppress_response": 0, "sid": sid, "data": list(payload),
    }
    t_end = time.time() + float(wait_pending_s)
    logged = False
    while True:
        if stopTask:
            raise RuntimeError("用户停止")
        if not logged:
            _log("[Tx] %02X %s" % (sid, _hex(payload)))
            logged = True
        resp = zcanpro.uds_request(bus_id, req)
        data = list((resp or {}).get("data") or [])
        if data:
            _log("[Rx] %s" % _hex(data[:40]))
        if len(data) >= 3 and data[0] == SID_NRC:
            if data[2] == NRC_RCRRP:
                if wait_pending_s <= 0 or time.time() >= t_end:
                    raise RuntimeError("NRC 0x78 超时")
                _log("NRC 0x78，等待中...")
                time.sleep(1.0)
                continue
            raise RuntimeError("NRC 0x%02X" % data[2])
        if not resp or not resp.get("result"):
            raise RuntimeError("无应答")
        if data[0] != (sid + SID_PR):
            raise RuntimeError("非正响应: %s" % _hex(data))
        return data


def read_did_string(bus_id, did):
    payload = [(did >> 8) & 0xFF, did & 0xFF]
    rx = uds_req(bus_id, SID_RDBI, payload)
    if len(rx) < 35:
        raise RuntimeError("DID 0x%04X 响应过短 (%d 字节): %s" % (did, len(rx), _hex(rx)))
    raw = rx[3:35]
    s = "".join(chr(b) if 0x20 <= b < 0x7F else "?" for b in raw).rstrip()
    return s


# ======== 主流程 ========

def run(bus_id):
    _log("======== 读取 APP 侧版本号 ========")
    _log("CAN ID: Tx 0x%08X  Rx 0x%08X" % (UDS_REQ_ID, UDS_RESP_ID))
    _log("")
    uds_init()
    results = []
    for did, name in DID_LIST:
        try:
            ver_str = read_did_string(bus_id, did)
            _log("DID 0x%04X [%s]: %s" % (did, name, ver_str))
            results.append((did, name, ver_str, None))
        except Exception as e:
            _log("DID 0x%04X [%s]: 读取失败 - %s" % (did, name, str(e)))
            results.append((did, name, None, str(e)))
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
