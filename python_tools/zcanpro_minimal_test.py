# -*- coding: utf-8 -*-
"""最小化测试: 只发一个 DID 0xF195，看 uds_request 是否能收到多帧响应"""

try:
    import zcanpro
except Exception:
    zcanpro = None

def z_main():
    if zcanpro is None:
        return

    buses = zcanpro.get_buses()
    if not buses:
        return
    bus_id = buses[0]["busID"]

    zcanpro.uds_init({
        "response_timeout_ms": 5000,
        "use_canfd": 0, "canfd_brs": 0,
        "trans_ver": 0,
        "fill_byte": 0xCC,
        "frame_type": 1,
        "trans_stmin_valid": 1, "trans_stmin": 1,
        "enhanced_timeout_ms": 30000,
    })

    req = {
        "src_addr": 0x18DA0D03,
        "dst_addr": 0x18DA030D,
        "suppress_response": 0,
        "sid": 0x22,
        "data": [0xF1, 0x95],
    }

    resp = zcanpro.uds_request(bus_id, req)

    zcanpro.uds_deinit()

    # 结果写到文件，绕过 ZCANPRO 日志
    import os
    p = os.path.join(os.path.dirname(__file__), "_probe_result.txt")
    with open(p, "w") as f:
        f.write("resp = %s\n" % str(resp))
        if resp:
            d = list(resp.get("data") or [])
            f.write("data = %s\n" % " ".join("%02X" % b for b in d))
            f.write("data_len = %d\n" % len(d))
            f.write("result = %s\n" % str(resp.get("result")))
        else:
            f.write("resp is None or empty\n")

def z_notify(type, obj):
    pass
