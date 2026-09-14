# -*- coding: utf-8 -*-
"""
ZCANPRO 脚本 — 探测可用 API + 测试不同 uds_init 配置
"""
import sys

try:
    import zcanpro
except ImportError:
    zcanpro = None

if zcanpro is None:
    sys.stdout.write("zcanpro 不可用\n")
    sys.exit(1)

# 1. 列出所有公开属性
attrs = [a for a in dir(zcanpro) if not a.startswith("_")]
sys.stdout.write("=== zcanpro 公开属性 (%d 个) ===\n" % len(attrs))
for a in attrs:
    obj = getattr(zcanpro, a)
    t = type(obj).__name__
    extra = ""
    if callable(obj):
        try:
            extra = "  args=%s" % str(obj.__code__.co_varnames[:obj.__code__.co_argcount])
        except Exception:
            pass
    sys.stdout.write("  %-30s %-10s%s\n" % (a, t, extra))
sys.stdout.flush()

# 2. 测试 uds_init 不同 trans_ver
sys.stdout.write("\n=== 测试不同 trans_ver ===\n")
buses = zcanpro.get_buses()
if not buses:
    sys.stdout.write("无 CAN 通道\n")
    sys.exit(1)

bus_id = buses[0]["busID"]
sys.stdout.write("bus_id = %s\n" % str(bus_id))

for tv in [0, 1, 2]:
    try:
        zcanpro.uds_init({
            "response_timeout_ms": 5000, "use_canfd": 0, "canfd_brs": 0,
            "trans_ver": tv, "fill_byte": 0xCC, "frame_type": 1,
            "trans_stmin_valid": 1, "trans_stmin": 1, "enhanced_timeout_ms": 30000,
        })
        req = {
            "src_addr": 0x18DA0D03, "dst_addr": 0x18DA030D,
            "suppress_response": 0, "sid": 0x22, "data": [0xF1, 0x95],
        }
        sys.stdout.write("[trans_ver=%d] 发送 22 F1 95 ...\n" % tv)
        sys.stdout.flush()
        resp = zcanpro.uds_request(bus_id, req)
        sys.stdout.write("  resp = %s\n" % str(resp))
        data = list((resp or {}).get("data") or [])
        sys.stdout.write("  data = %s\n" % " ".join("%02X" % b for b in data))
        sys.stdout.flush()
        zcanpro.uds_deinit()
        break  # 成功就停
    except Exception as e:
        sys.stdout.write("  失败: %s\n" % str(e))
        sys.stdout.flush()
        try:
            zcanpro.uds_deinit()
        except Exception:
            pass

# 3. 试一下 zcanpro 是否有 can_send / can_recv / tx / rx 等
sys.stdout.write("\n=== 搜索原始 CAN 函数 ===\n")
can_names = [a for a in attrs if any(k in a.lower() for k in ["can", "send", "recv", "tx", "rx", "frame", "raw"])]
for a in can_names:
    sys.stdout.write("  %s\n" % a)
if not can_names:
    sys.stdout.write("  未发现原始 CAN 相关函数\n")
sys.stdout.flush()
