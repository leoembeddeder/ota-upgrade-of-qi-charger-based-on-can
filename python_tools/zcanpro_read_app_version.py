# -*- coding: utf-8 -*-
"""
ZCANPRO 脚本 — 读取 APP 侧版本号（ISO-TP 手动重组版）

绕过 ZCANPRO UDS 层，直接收发原始 CAN 帧并手动重组 ISO-TP 多帧响应。
解决 ZCANPRO uds_request 对多帧 DID 响应"无应答"的问题。

读取三个 DID（32 字节 ASCII，空格填充）：
  0xF195  APP 软件版本 (SW_VERSION)
  0xF180  Bootloader 版本 (BL_VERSION)
  0xF193  硬件版本 (HW_VERSION)

不需要编程会话或安全解锁，任意会话可读。

用法: ZCANPRO → 高级功能 → 扩展脚本 → 打开本文件
"""

import sys
import time

try:
    import zcanpro
except ImportError:
    zcanpro = None

# ======== CAN / ISO-TP 常量 ========
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

ISO_TP_TIMEOUT_S = 5.0

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


# ======== ISO-TP 手动收发 ========

def isotp_xfer(req_payload):
    """
    手动 ISO-TP 收发：发送单帧请求，接收并重组多帧响应。
    返回完整 UDS payload（去掉 ISO-TP PCI 字节）。
    """
    # --- 发送 SF (Single Frame) ---
    sf = [0x00] * 8
    dl = len(req_payload)
    sf[0] = dl & 0x0F
    for i in range(dl):
        if i + 1 < 8:
            sf[i + 1] = req_payload[i]
    _log("[Tx] %s" % _hex(sf))
    zcanpro.send(UDS_REQ_ID, sf, 8)

    # --- 等待响应 ---
    t0 = time.time()
    rx_payload = bytearray()
    expect_len = 0
    next_seq = 0
    state = "wait_ff"  # wait_ff -> wait_cf
    cf_count = 0

    while (time.time() - t0) < ISO_TP_TIMEOUT_S:
        if stopTask:
            raise RuntimeError("用户停止")

        frames = zcanpro.receive()
        if not frames:
            time.sleep(0.002)
            continue

        for f in frames:
            fid = f.get("id", 0)
            data = f.get("data", [])
            if fid != UDS_RESP_ID or len(data) == 0:
                continue

            pci = (data[0] >> 4) & 0x0F

            if pci == 0:
                # Single Frame
                sf_len = data[0] & 0x0F
                _log("[Rx] SF: %s" % _hex(data[:8]))
                return list(data[1:1 + sf_len])

            elif pci == 1:
                # First Frame
                expect_len = ((data[0] & 0x0F) << 8) | data[1]
                rx_payload = bytearray(data[2:8])
                next_seq = 1
                state = "wait_cf"
                cf_count = 0
                _log("[Rx] FF: DL=%d, data=%s" % (expect_len, _hex(data[2:8])))

                # 发送 Flow Control
                fc = [0x30, 0x00, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC]
                _log("[Tx] FC: %s" % _hex(fc))
                zcanpro.send(UDS_REQ_ID, fc, 8)

            elif pci == 2 and state == "wait_cf":
                # Consecutive Frame
                seq = data[0] & 0x0F
                if seq != (next_seq & 0x0F):
                    _log("[Rx] CF 序列号不匹配: 期望 %d, 收到 %d" % (next_seq & 0x0F, seq))
                    continue
                next_seq += 1
                cf_data = data[1:8]
                rx_payload.extend(cf_data)
                cf_count += 1
                _log("[Rx] CF#%d seq=%d: %s [%d/%d]" % (
                    cf_count, seq, _hex(cf_data), len(rx_payload), expect_len))

                if len(rx_payload) >= expect_len:
                    result = list(rx_payload[:expect_len])
                    _log("[Rx] 重组完成: %d 字节" % len(result))
                    return result

    raise RuntimeError("ISO-TP 超时 (%.1fs): state=%s, 收到 %d/%d 字节, CF=%d" % (
        ISO_TP_TIMEOUT_S, state, len(rx_payload), expect_len, cf_count))


def read_did(bus_id, did):
    """读取单个 DID，返回 UDS payload 列表。"""
    payload = [(did >> 8) & 0xFF, did & 0xFF]
    rx = isotp_xfer(payload)

    if len(rx) < 3:
        raise RuntimeError("DID 0x%04X 响应过短: %s" % (did, _hex(rx)))
    if rx[0] == SID_NRC:
        raise RuntimeError("DID 0x%04X NRC: 0x%02X 0x%02X" % (did, rx[1], rx[2]))
    if rx[0] != (SID_RDBI + SID_PR):
        raise RuntimeError("DID 0x%04X 非正响应: %s" % (did, _hex(rx)))
    return rx


def read_did_string(bus_id, did):
    """读取 DID 并提取 32 字节 ASCII 版本字符串。"""
    rx = read_did(bus_id, did)
    if len(rx) < 35:
        raise RuntimeError("DID 0x%04X 数据不足 32 字节: %d 字节" % (did, len(rx) - 3))
    raw = rx[3:35]
    s = "".join(chr(b) if 0x20 <= b < 0x7F else "?" for b in raw).rstrip()
    return s


# ======== 主流程 ========

def run(bus_id):
    _log("======== 读取 APP 侧版本号 (ISO-TP 手动版) ========")
    _log("CAN ID: Tx 0x%08X  Rx 0x%08X" % (UDS_REQ_ID, UDS_RESP_ID))
    _log("")

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

    _log("======== APP 版本读取工具 (ISO-TP 手动版) ========")
    _log("支持 DID: 0xF195(SW) / 0xF180(BL) / 0xF193(HW)")
    _log("")

    buses = zcanpro.get_buses()
    if not buses:
        _log("请先打开 CAN 通道 (250kbps, 扩展帧)")
        return

    bus_id = buses[0]["busID"]

    try:
        run(bus_id)
    except Exception as e:
        _log("失败: " + str(e))
