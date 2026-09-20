# -*- coding: utf-8 -*-
"""
ZCANPRO 扩展脚本 — Boot 诊断帧抓取（开机决策链 M1~M4）【通信层 v2】

配套 Boot 诊断标记版（commit 60d1137）：复位设备 → 抓取开机决策链四点
CAN 标记帧（ID=0x18FF480D）→ 逐帧人话解码 → 决策链串联摘要。

通信层 v2（2026-09-20 抢修，对齐版本工具/升级脚本已验证模式）：
  v1 通信路径与真机 ZCANPRO 引擎不合（10:33 实证：同机同通道版本工具
  全通、本脚本全聋）。v2 照抄三个真机实证模式，不自创：
  ① uds_init 配置+3E 00 探活+uds_request 读 DID = zcanpro_read_app_version.py
    （10:33 实机可用）。关键教训：ZCANPRO 必须先 uds_init 才开接收，
    否则 receive 恒为 (1,[])（该工具 run() 注释实证）。
  ② 11 01 复位发送+51 01 等待 = zcanpro_ext_ota_auto.py uds_ecu_reset
    （07:08 实机可用）：非 suppress、uds_request 等正响应、≤3 次重试。
  ③ raw 收发 = zcanpro_read_app_version.py 的 can_send/can_recv/_make_frame/
    _unwrap_receive/_parse_one_frame（transmit(list)优先四形态回退+
    完整扩展帧标志+首3帧原始 repr 日志）。
  通道顺序照版本工具 raw 分支：UDS 操作 → sleep(0.05) → deinit → raw 监听。

用法（ZCANPRO → 高级功能 → 扩展脚本 → 打开本文件 → 运行）：
  前提：设备 Boot 已烧录 60d1137 诊断标记版构建；CAN 通道 250kbps
  Classical CAN 扩展帧。
  流程：①uds_init（版本工具同款）→ 3E 00 探活 → ②读基线 22 2113 +
  22 F195（版本工具同款）→ ③复位 11 01（升级脚本同款，非 suppress ≤3
  次等 51 01）→ ④deinit → raw 捕获窗口 ≥30s：打印每一帧原始数据
  （含非诊断帧，无需上位机过滤），0x18FF480D 额外人话解码 → ⑤复位后
  复读 DID → ⑥总线活动统计 + 零帧二分/UDS 通 raw 不通降级提示 +
  决策链摘要 + 一致性对照。

解码表与 60d1137 终审规格逐字节一致（boot_safe_mode.h BOOT_DIAG 注释）：
  M1 0xA1: [app_valid][meta_src][magic_ok][ver_ok][crc_ok]
  M2 0xA2: [copy result][detail]
  M3 0xA3: [pass][fail_step][target 0=Backup/1=App]
  M4 0xA4: [app_addr LE 4B] (App entry 0x08004100)
  帧格式：DLC=8，[标记 0xA1~0xA4][数据...][0xCC 填充]。

本脚本为独立诊断工具：不签名、不写设备、无私钥/固件路径引用；
升级脚本 zcanpro_ext_ota_auto.py 与版本工具 zcanpro_read_app_version.py
只读参考（照抄它们的通信模式，不修改它们）。
"""

import sys
import time

try:
    import zcanpro
except ImportError:
    zcanpro = None

# ======== 配置常量 ========
DIAG_CAN_ID = 0x18FF480D          # Boot 诊断标记帧 ID（60d1137）
UDS_REQ_ID = 0x18DA0D03
UDS_RESP_ID = 0x18DA030D
EXT_FLAG = 0x80000000
CAPTURE_WINDOW_S = 30.0           # 复位后捕获窗口（秒，可调）
RESET_RETRIES = 3                 # 11 01 重试次数（升级脚本 uds_ecu_reset 同款）
RESET_WAIT_S = 2.0                # 每次复位命令等待上限（日志节奏用）
POLL_INTERVAL_S = 0.01
SID_TP = 0x3E
SID_RDBI = 0x22
SID_ER = 0x11

# ======== 解码语义表（60d1137 终审 spec，勿自行发挥） ========
META_SRC = {
    0x00: "主区0x0801C000生效",
    0x01: "备区0x0801C800恢复生效",
    0x02: "defaults兜底(双区无效)",
    0xFF: "未记录(init未执行)",
}
FAIL_STEP = {
    0: "向量门失败或未执行校验",
    1: "magic校验失败",
    2: "image_length越界",
    3: "CRC32校验失败",
    4: "Reset不在本槽",
    5: "公钥缺失/无效",
    6: "ECDSA签名失败",
}

stopTask = False
_tx_mode = None      # 版本工具同款：记住本机可用的发送形态
_rx_logged = 0       # 版本工具同款：前 3 次 receive 打原始 repr


# ======== 基础工具 ========
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
    if b is None:
        return []
    if sys.version_info[0] >= 3:
        return list(b)
    return [ord(c) for c in b]


def _pad8(data):
    d = [int(x) & 0xFF for x in list(data)]
    while len(d) < 8:
        d.append(0xCC)
    return d[:8]


def _slot_name(v):
    """Region label (single-App arch): 0=App region, 1=Backup region."""
    return {0: "App区", 1: "备份区"}.get(int(v) & 0xFF,
                                     "未知(0x%02X)" % (int(v) & 0xFF))


def _bool1(v):
    v = int(v) & 0xFF
    if v == 1:
        return "过"
    if v == 0:
        return "不过"
    return "异常(0x%02X)" % v


def _addr_target(addr):
    a = int(addr) & 0xFFFFFFFF
    if a == 0x08004100:
        return "App区"
    if a == 0x08010100:
        return "历史B槽地址(已废弃)"
    return "非标准目标(0x%08X)" % a


def z_notify(type, obj):
    global stopTask
    if type == "stop":
        stopTask = True


# ======== 纯解码函数（帧字节→结构/人话，无 IO 无全局依赖） ========
def parse_diag_frame(data):
    """8 字节诊断帧 → 结构化 dict；非诊断帧/未知标记/空帧 → None。"""
    b = [int(x) & 0xFF for x in (data or [])]
    if len(b) < 2:
        return None
    m = b[0]
    if m == 0xA1:
        if len(b) < 6:
            return {"marker": 0xA1, "raw": b, "error": "M1 帧长度不足6"}
        return {"marker": 0xA1, "app_valid": b[1], "meta_src": b[2],
                "magic_ok": b[3], "ver_ok": b[4], "crc_ok": b[5]}
    if m == 0xA2:
        if len(b) < 3:
            return {"marker": 0xA2, "raw": b, "error": "M2 帧长度不足3"}
        # M2 wire 统一序 b1=copy result b2=detail（OTA-ARCH-0920-D5，
        # 与 boot_safe_mode.h 文档及 boot_safe_mode.c 实现一致）
        return {"marker": 0xA2, "ret": b[1], "info": b[2]}
    if m == 0xA3:
        if len(b) < 4:
            return {"marker": 0xA3, "raw": b, "error": "M3 帧长度不足4"}
        return {"marker": 0xA3, "passed": b[1], "fail_step": b[2], "target": b[3]}
    if m == 0xA4:
        if len(b) < 5:
            return {"marker": 0xA4, "raw": b, "error": "M4 帧长度不足5"}
        addr = b[1] | (b[2] << 8) | (b[3] << 16) | (b[4] << 24)
        return {"marker": 0xA4, "addr": addr}
    return None


def decode_diag_frame(data):
    """8 字节诊断帧 → 人话一行。纯函数：无 IO、无全局状态。"""
    p = parse_diag_frame(data)
    if p is None:
        return "[诊断] 无法解码的帧：%s（非0xA1~0xA4标记或空帧）" % _hex(data or [])
    if p.get("error"):
        return "[诊断] %s（原始：%s）" % (p["error"], _hex(p.get("raw", [])))
    m = p["marker"]
    if m == 0xA1:
        return ("[诊断] M1 metadata读取：app_valid=%s；来源=%s；magic=%s 版本=%s CRC=%s"
                % (_bool1(p["app_valid"]),
                   META_SRC.get(p["meta_src"], "未知(0x%02X)" % p["meta_src"]),
                   _bool1(p["magic_ok"]), _bool1(p["ver_ok"]), _bool1(p["crc_ok"])))
    if m == 0xA2:
        if p["ret"] == 1:
            return "[诊断] M2 搬运结果：成功（Backup→App 已提交）"
        if p["ret"] == 0xFF:
            return ("[诊断] M2 搬运结果：失败 detail=0x%02X（flag 保留，下轮开机重试）"
                    % p["info"])
        if p["info"] == 0xFF:
            return "[诊断] M2 搬运结果：无待搬运固件"
        return "[诊断] M2 搬运序列开始"
    if m == 0xA3:
        target = {0: "Backup区(源)", 1: "App区"}.get(p["target"],
                                                     "未知(0x%02X)" % p["target"])
        if p["fail_step"] == 0xFF:
            return "[诊断] M3 验签开始：目标=%s" % target
        if p["passed"]:
            return "[诊断] M3 验签：通过 目标=%s" % target
        return ("[诊断] M3 验签：失败 关卡%d(%s) 目标=%s"
                % (p["fail_step"],
                   FAIL_STEP.get(p["fail_step"], "未知关卡"), target))
    if m == 0xA4:
        return "[诊断] M4 跳转目标：0x%08X→%s" % (p["addr"], _addr_target(p["addr"]))
    return "[诊断] 未知标记 0x%02X" % m


def _parse_did_positive(rx, did):
    """UDS RDBI 正响应判定（版本工具 parse_did_string 同款语义，纯函数）：
    NRC / safe-mode 标记 / 非正响应 → 抛异常；通过 → 返回 rx。"""
    if rx and len(rx) >= 3 and rx[0] == 0x7F:
        raise RuntimeError("NRC 0x%02X" % rx[2])
    if (not rx) or len(rx) < 1 or rx[0] != 0x62:
        raise RuntimeError("非正响应: %s" % _hex(rx))
    if len(rx) >= 5 and rx[1] == 0x21 and rx[3] == 0xFE:
        raise RuntimeError("Boot safe mode fail_step=%d" % rx[4])
    return rx


def _parse_version_ascii(rx):
    """0xF195 应答 → 版本字符串（版本工具 parse_did_string 同款，纯函数）：
    32B ASCII 右补空格 rstrip；短应答形态兜底。"""
    if rx and len(rx) >= 35:
        raw = rx[3:35]
        s = "".join(chr(b) if 0x20 <= b < 0x7F else "?" for b in raw).rstrip()
        return s or None
    if rx and len(rx) >= 4:
        vb = bytes(bytearray(rx[3:])).split(b"\x00")[0]
        s = vb.decode("ascii", "ignore").strip()
        return s or None
    return None


def summarize_bus(events):
    """events=[(t_rel, can_id, data),...] → 总线活动统计 dict。纯函数。"""
    ids = {}
    diag = 0
    for _t, cid, _d in events:
        cidi = int(cid) & 0x1FFFFFFF
        ids[cidi] = ids.get(cidi, 0) + 1
        if cidi == DIAG_CAN_ID:
            diag += 1
    return {"total": len(events), "ids": ids, "diag": diag}


def format_bus_stats(st):
    """统计 dict → 人话统计行（含按 ID 分布）。纯函数。"""
    parts = ["ID=0x%08X ×%d" % (k, v) for k, v in sorted(st["ids"].items())]
    return ("[诊断] 总线活动统计：总帧数=%d，诊断帧(0x18FF480D)=%d；"
            "按 ID 分布：%s"
            % (st["total"], st["diag"], "，".join(parts) if parts else "无"))


def bus_silent_hint():
    """总帧数=0 且 UDS 也不通：总线/设备/物理层排查。"""
    return ("[诊断] 总线完全无任何帧（连非诊断流量都没有）且 UDS 路径也无应答"
            "→ 按物理层排查：设备上电/CAN 接线/CAN 通道参数（250kbps 扩展帧）；"
            "若确认已烧 60d1137 诊断版 Boot 且设备已上电仍零帧，回报 agent:main。"
            "注意：零帧≠决策证据——不能据此判定 Boot 决策行为。")


def traffic_no_diag_hint(st):
    """总帧数>0 且诊断帧=0：有流量无诊断帧 → Boot 版本/开机事件二分。"""
    parts = ["ID=0x%08X ×%d" % (k, v) for k, v in sorted(st["ids"].items())]
    return ("[诊断] 总线有流量但无 0x18FF480D 诊断帧（总帧数=%d，ID 分布：%s）"
            "→ 设备 Boot 可能非诊断版（未烧 60d1137 构建）或本窗口内未发生"
            "开机事件——断电重试一次再看（断电重试期间本窗口继续监听并"
            "打印全部帧，无需任何上位机操作）。注意：无诊断帧≠决策证据。"
            % (st["total"], "，".join(parts) if parts else "无"))


def raw_dead_hint():
    """UDS 通但 raw 零帧：raw 接收在本引擎不可用（通信层 v2 降级兜底）。"""
    return ("[诊断] UDS 路径已确认设备在线（基线读值或 51 01 有应答）但 raw "
            "监听 0 帧 → raw 接收在本 ZCANPRO 引擎上疑似不可用（通信层 v2 "
            "已照抄版本工具 raw 形态仍不通时的结论）——请断电重试后重跑一"
            "次看统计；若重跑仍是『UDS 通/raw 零帧』，回报 agent:main 并附"
            "本日志（现场可用版本工具 raw 嗅探分支对照验证）。"
            "注意：零帧≠决策证据。")


def summarize_chain(parsed_frames, pre, post):
    """决策链串联摘要。parsed_frames=parse_diag_frame 结果序列；
    pre/post=(slot, ver) 或 (None, None)。返回人话行列表。纯函数。"""
    m1 = next((p for p in parsed_frames if p and p.get("marker") == 0xA1 and "error" not in p), None)
    m2 = next((p for p in parsed_frames if p and p.get("marker") == 0xA2 and "error" not in p), None)
    m3s = [p for p in parsed_frames if p and p.get("marker") == 0xA3 and "error" not in p]
    m4 = next((p for p in parsed_frames if p and p.get("marker") == 0xA4 and "error" not in p), None)

    if m1:
        s1 = "读记录=app_valid=%s(%s,magic:%s/ver:%s/crc:%s)" % (
            _bool1(m1["app_valid"]),
            META_SRC.get(m1["meta_src"], "src=0x%02X" % m1["meta_src"]),
            _bool1(m1["magic_ok"]), _bool1(m1["ver_ok"]), _bool1(m1["crc_ok"]))
    else:
        s1 = "读记录=无M1帧"
    if m2:
        if m2["ret"] == 1:
            s2 = "搬运=成功"
        elif m2["ret"] == 0xFF:
            s2 = "搬运=失败(detail=0x%02X)" % m2["info"]
        elif m2["info"] == 0xFF:
            s2 = "搬运=无待搬运固件"
        else:
            s2 = "搬运=序列开始"
    else:
        s2 = "搬运=无M2帧"
    if m3s:
        parts = []
        for p in m3s:
            target = {0: "Backup", 1: "App"}.get(p["target"], "0x%02X" % p["target"])
            if p["fail_step"] == 0xFF:
                parts.append("%s验签中" % target)
            elif p["passed"]:
                parts.append("%s通过" % target)
            else:
                parts.append("%s失败(关卡%d:%s)" % (
                    target, p["fail_step"],
                    FAIL_STEP.get(p["fail_step"], "未知")))
        s3 = "验签=" + "；".join(parts)
    else:
        s3 = "验签=无M3帧"
    if m4:
        s4 = "实跳=0x%08X(%s)" % (m4["addr"], _addr_target(m4["addr"]))
    else:
        s4 = "实跳=无M4帧"

    lines = ["[摘要] 决策链串联：%s → %s → %s → %s" % (s1, s2, s3, s4)]

    pre_s = "槽=%s 版本=%s" % (_slot_name(pre[0]) if pre[0] is not None else "未读到",
                              pre[1] or "未读到") if pre else "未读到"
    post_s = "槽=%s 版本=%s" % (_slot_name(post[0]) if post[0] is not None else "未读到",
                               post[1] or "未读到") if post else "未读到"
    lines.append("[摘要] 复位前基线：%s；复位后实读：%s" % (pre_s, post_s))

    if m4 and post and post[0] is not None:
        m4_slot = None
        if m4["addr"] == 0x08004100:
            m4_slot = 0
        elif m4["addr"] == 0x08010100:
            m4_slot = 1
        if m4_slot is None:
            lines.append("[摘要] 一致性对照：M4 目标 0x%08X 非标准槽地址，无法对照"
                         "（结合 M2/M3 判读）" % m4["addr"])
        elif m4_slot == (post[0] & 0xFF):
            lines.append("[摘要] 一致性对照：一致（Boot 实跳=%s槽 与 设备自报 0x2113=%s槽 吻合）"
                         % (_slot_name(m4_slot), _slot_name(post[0])))
        else:
            lines.append("[摘要] 一致性对照：!!!! 不一致 !!!! Boot 实跳=%s槽（M4=0x%08X）"
                         "而设备自报 0x2113=%s槽——结合 M2/M3 判读（回落形态/自报失真/"
                         "观测窗口错位），必要时断电重抓"
                         % (_slot_name(m4_slot), m4["addr"], _slot_name(post[0])))
    else:
        lines.append("[摘要] 一致性对照：信息不足（缺 M4 帧或复位后 0x2113 读值），"
                     "按上方各帧明细判读")
    return lines


# ======== 通信层 v2（照抄真机已验证模式，不自创） ========
# 来源①：zcanpro_read_app_version.py（10:33 真机同机同通道实证可用）
#   _uds_init / _make_frame / can_send / _parse_one_frame / _unwrap_receive /
#   can_recv / uds_req / wake_mcu / parse_did_string
# 来源②：zcanpro_ext_ota_auto.py uds_ecu_reset（07:08 真机实证 11 01→51 01）

def _uds_init():
    """版本工具 _uds_init 同款配置（10:33 实证）：不靠 suppress；STmin=10
    避免 CF 乱序。关键教训（版本工具 run() 注释）：ZCANPRO 必须先
    uds_init 才会开接收，不 init 时 receive 恒为 (1,[])。"""
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
    """版本工具 _make_frame 同款：ZLG bit31=1 扩展帧 + 完整标志位集。"""
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
    """版本工具 can_send 同款：transmit(list)/transmit(dict)/send(list)/
    send(dict) 四形态回退，_tx_mode 记住本机可用形态。"""
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
            return True
        except Exception as e:
            last = e
    _log("[诊断] CAN 发送失败（四形态均不通）: %s" % last)
    return False


def _as_int(x):
    try:
        return int(x)
    except Exception:
        return None


def _parse_one_frame(f):
    """版本工具 _parse_one_frame 同款：dict/list/tuple/object → (cid, data)。"""
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
    """版本工具 _unwrap_receive 同款：(status,[frames]) / [frames] / 单帧。"""
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
    """版本工具 can_recv 同款：receive(bus_id)→TypeError 回退 receive()；
    前 3 次打印 receive 原始 repr（现场判 R3 的关键证据）。"""
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


def _recv_frames_min(bus_id):
    """raw 收帧（诊断窗口用）：通信层 v2 = 版本工具 can_recv 输出形态。"""
    for cid, dat in can_recv(bus_id):
        yield (cid, dat)


def _uds_request(bus_id, sid, payload, timeout_note=""):
    """版本工具 uds_req 同款（10:33 实证）：uds_request dict + result 检查。"""
    req = {
        "src_addr": UDS_REQ_ID, "dst_addr": UDS_RESP_ID,
        "suppress_response": 0, "sid": sid, "data": list(payload),
    }
    _log("[Tx uds] %02X %s" % (sid, _hex(payload)))
    resp = zcanpro.uds_request(bus_id, req)
    data = list((resp or {}).get("data") or [])
    if data:
        _log("[Rx uds] %s" % _hex(data[:40]))
    if (not resp) or (not resp.get("result")):
        raise RuntimeError("uds 无应答%s: %s"
                           % (timeout_note, (resp or {}).get("result_msg", "")))
    return data


def _wake_probe(bus_id):
    """版本工具 wake_mcu 同款：3E 00 等 7E（第一帧可能被 SIT1145
    Standby 的 WUP 吃掉，故发两次）。"""
    for i in range(1, 3):
        if stopTask:
            raise RuntimeError("用户停止脚本")
        try:
            rx = _uds_request(bus_id, SID_TP, [0x00], " (探活第%d次)" % i)
            if rx and rx[0] == (SID_TP + 0x40):
                _log("[诊断] MCU 已在线 (7E 00)")
                return True
        except Exception as e:
            _log("[诊断] 探活 3E 00 %d/2: %s" % (i, e))
            time.sleep(0.15)
    return False


def read_slot_ver(bus_id, label):
    """读 22 2113 + 22 F195（版本工具 uds_req + parse 同款语义）：
    NRC/safe-mode 判定进 _parse_did_positive；单项失败不拦截流程。"""
    slot = None
    ver = None
    try:
        rx = _uds_request(bus_id, SID_RDBI, [0x21, 0x13], " (%s 2113)" % label)
        rx = _parse_did_positive(rx, 0x2113)
        if len(rx) >= 4 and rx[1] == 0x21 and rx[2] == 0x13:
            slot = rx[3]
            _log("[%s] 0x2113=%s" % (label, _slot_name(slot)))
        else:
            _log("[%s] 0x2113 应答异常: %s" % (label, _hex(rx[:8])))
    except Exception as e:
        _log("[%s] 0x2113 读取失败: %s" % (label, e))
    try:
        rx = _uds_request(bus_id, SID_RDBI, [0xF1, 0x95], " (%s F195)" % label)
        rx = _parse_did_positive(rx, 0xF195)
        ver = _parse_version_ascii(rx)
        _log("[%s] 0xF195=%s" % (label, ver or "未读到"))
    except Exception as e:
        _log("[%s] 0xF195 读取失败: %s" % (label, e))
    return slot, ver


def _reset_device(bus_id):
    """升级脚本 uds_ecu_reset 同款（07:08 实证）：非 suppress 11 01，
    uds_request 等 51 01 正响应，≤3 次重试间隔 0.5s；全败返回 False
    （不中断流程，raw 窗口继续供断电重试观测）。"""
    last_err = None
    for attempt in range(1, RESET_RETRIES + 1):
        if stopTask:
            raise RuntimeError("用户停止脚本")
        try:
            _log("[诊断] ECUReset 11 01（非 suppress，等 51 01 正响应；第 %d/%d 次）"
                 % (attempt, RESET_RETRIES))
            rx = _uds_request(bus_id, SID_ER, [0x01], " (复位第%d次)" % attempt)
            if rx and rx[0] == 0x51:
                _log("[诊断] 收到 51 01，MCU 复位中——切入 raw 监听抓 M1~M4")
                return True
            _log("[诊断] 11 01 第 %d/%d 次应答异常: %s"
                 % (attempt, RESET_RETRIES, _hex(rx[:8])))
        except Exception as e:
            last_err = e
            _log("[诊断] 11 01 第 %d/%d 次未收到 51 01: %s"
                 % (attempt, RESET_RETRIES, e))
        if attempt < RESET_RETRIES:
            time.sleep(0.5)
    _log("[诊断] !!!!! 11 01 三次均未收到 51 01（%s）!!!!! 设备可能无响应"
         "或链路异常；可手动断电重试——断电重试期间 raw 窗口继续监听并"
         "打印全部帧，无需任何上位机操作" % (last_err or "应答异常"))
    return False


# ======== 主流程 ========
def z_main():
    global stopTask
    stopTask = False
    if zcanpro is None:
        _log("本脚本需在 ZCANPRO 扩展脚本引擎内运行（宿主机自测请直接 python3 运行）")
        return
    _log("======== Boot 诊断帧抓取（CAN ID 0x18FF480D，配套 60d1137）========")
    _log("[通信层 v2（对齐版本工具/升级脚本已验证模式）]")
    buses = zcanpro.get_buses()
    _log("bus = " + str(buses))
    if not buses:
        _log("请先打开 CAN 通道 250kbps Classical CAN 扩展帧")
        return
    bus_id = buses[0]["busID"]
    _log("zcanpro API: " + ", ".join(a for a in dir(zcanpro) if not a.startswith("_")))

    uds_alive = False

    # 1) UDS init（版本工具同款配置——ZCANPRO 先 init 才开接收）
    _uds_init()

    # 2) 3E 00 探活（版本工具 wake_mcu 同款）
    if _wake_probe(bus_id):
        uds_alive = True

    # 3) 基线读值（版本工具 uds_req + parse 同款）
    pre = read_slot_ver(bus_id, "基线-复位前")
    if pre[0] is not None or pre[1] is not None:
        uds_alive = True

    # 4) 复位（升级脚本 uds_ecu_reset 同款：11 01 非 suppress ≤3 次等 51 01）
    reset_ok = _reset_device(bus_id)
    if reset_ok:
        uds_alive = True

    # 5) 通道切换照版本工具 raw 分支顺序：sleep(0.05) → deinit → raw 监听
    time.sleep(0.05)
    _uds_deinit()
    t_start = time.time()
    _log("[诊断] raw 监听已建立（通信层 v2；51 01 后设备复位在途，"
         "M1~M4 为毫秒级窗口；断电重试期间同样在监听）")

    events = []          # (t_rel, cid, data)——窗口内全部帧，统计唯一真相源
    parsed = []          # parse_diag_frame 结果序列

    def _collect(t0):
        """收帧一轮：打印每一帧原始数据（诊断帧额外人话解码）。"""
        for cid, dat in _recv_frames_min(bus_id):
            t_rel = time.time() - t0
            events.append((t_rel, cid, dat))
            _log("[Rx] T+%7.3fs ID=0x%08X DLC=%d data=%s"
                 % (t_rel, cid & 0xFFFFFFFF, len(dat), _hex(dat)))
            if (cid & 0x1FFFFFFF) == DIAG_CAN_ID:
                parsed.append(parse_diag_frame(dat))
                _log(decode_diag_frame(dat))

    # 6) 捕获窗口：监听建立后 ≥CAPTURE_WINDOW_S，全帧打印+统计保留
    while (time.time() - t_start) < CAPTURE_WINDOW_S and not stopTask:
        _collect(t_start)
        time.sleep(POLL_INTERVAL_S)
    _log("[诊断] 捕获窗口结束（%.1fs）" % CAPTURE_WINDOW_S)
    st = summarize_bus(events)
    _log(format_bus_stats(st))
    if st["diag"] == 0:
        if st["total"] == 0:
            if uds_alive:
                _log(raw_dead_hint())      # 降级兜底：UDS 通/raw 不通
            else:
                _log(bus_silent_hint())
        else:
            _log(traffic_no_diag_hint(st))

    # 7) APP 回来后复读 DID（重新 init，版本工具同款配置）
    _uds_init()
    post = read_slot_ver(bus_id, "基线-复位后")

    # 8) 决策链摘要
    _log("======== 决策链摘要 ========")
    for line in summarize_chain(parsed, pre, post):
        _log(line)
    if stopTask:
        _log("[诊断] （用户中途停止，摘要基于已捕获帧）")


# ======== 宿主机合成流自测（无 zcanpro 时运行；铁律：交付前自测） ========
def _selftest():
    cases = []

    def check(name, text, expect_sub):
        cases.append((name, expect_sub in text, text))

    def dcheck(name, frame, expect_sub):
        s = decode_diag_frame(frame)
        cases.append((name, expect_sub in s, s))

    # ---- 合成流三场景（00d54dd 铁律保留）----
    ev1 = [(0.011, UDS_RESP_ID, [0x02, 0x51, 0x01, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC]),
           (0.052, DIAG_CAN_ID, [0xA1, 0x01, 0x00, 0x01, 0x01, 0x01, 0xCC, 0xCC]),
           (0.054, DIAG_CAN_ID, [0xA2, 0x01, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC]),
           (0.100, 0x18FF260D, [0x01, 0x41, 0x42, 0x54, 0x00, 0x00, 0xA5, 0x00])]
    st1 = summarize_bus(ev1)
    s1 = format_bus_stats(st1)
    cases.append(("场景1统计：混流含诊断帧(总4/诊断2)",
                  ("总帧数=4，诊断帧(0x18FF480D)=2" in s1)
                  and ("ID=0x18FF480D ×2" in s1), s1))
    cases.append(("场景1：诊断帧>0 不触发零帧提示", st1["diag"] == 2,
                  "diag=%d" % st1["diag"]))
    ev2 = [(0.010, UDS_RESP_ID, [0x02, 0x51, 0x01, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC]),
           (0.080, 0x18FF260D, [0x01, 0x41, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05]),
           (0.150, 0x18FF260D, [0x01, 0x41, 0x00, 0x02, 0x03, 0x04, 0x05, 0x06])]
    st2 = summarize_bus(ev2)
    h2 = traffic_no_diag_hint(st2)
    cases.append(("场景2统计：混流无诊断帧(总3/诊断0)",
                  "总帧数=3，诊断帧(0x18FF480D)=0" in format_bus_stats(st2),
                  format_bus_stats(st2)))
    cases.append(("场景2提示：有流量无诊断帧二分+ID分布+免上位机",
                  ("总线有流量但无 0x18FF480D 诊断帧" in h2)
                  and ("ID=0x18FF260D ×2" in h2)
                  and ("无需任何上位机操作" in h2), h2))
    st3 = summarize_bus([])
    h3 = bus_silent_hint()
    cases.append(("场景3统计：全静默(总0/诊断0/分布无)",
                  "总帧数=0，诊断帧(0x18FF480D)=0；按 ID 分布：无"
                  in format_bus_stats(st3), format_bus_stats(st3)))
    cases.append(("场景3提示：总线死+物理层+回报agent:main+零帧≠证据",
                  ("总线完全无任何帧" in h3) and ("回报 agent:main" in h3)
                  and ("零帧≠决策证据" in h3), h3))

    # ---- 新增：UDS 基线成功形态（通信层 v2 纯函数）----
    ok2113 = [0x62, 0x21, 0x13, 0x00, 0xCC, 0xCC, 0xCC, 0xCC]
    cases.append(("UDS基线：2113 正响应通过判定(槽A)",
                  _parse_did_positive(list(ok2113), 0x2113)[3] == 0x00,
                  "rx[3]=0x00 → A"))
    ver_rx = [0x62, 0xF1, 0x95] + [ord(c) for c in "QC_JYF_FW_1.1.1"] + \
        [0x20] * (32 - len("QC_JYF_FW_1.1.1")) + [0xCC]
    cases.append(("UDS基线：F195 32B ASCII 解析(版本工具同款)",
                  _parse_version_ascii(ver_rx) == "QC_JYF_FW_1.1.1",
                  str(_parse_version_ascii(ver_rx))))
    try:
        _parse_did_positive([0x7F, 0x22, 0x31], 0x2113)
        cases.append(("UDS基线：NRC 应答抛异常", False, "未抛"))
    except RuntimeError as e:
        cases.append(("UDS基线：NRC 应答抛异常", "NRC 0x31" in str(e), str(e)))
    try:
        _parse_did_positive([0x62, 0x21, 0x13, 0xFE, 0x06, 0xCC], 0x2113)
        cases.append(("UDS基线：safe-mode 标记抛异常", False, "未抛"))
    except RuntimeError as e:
        cases.append(("UDS基线：safe-mode 标记抛异常",
                      "fail_step=6" in str(e), str(e)))
    h4 = raw_dead_hint()
    cases.append(("降级兜底：UDS通/raw零帧提示",
                  ("UDS 路径已确认设备在线" in h4)
                  and ("断电重试后重跑" in h4)
                  and ("回报 agent:main" in h4)
                  and ("零帧≠决策证据" in h4), h4))

    # ---- 解码/摘要回归（保留）----
    dcheck("M1 app_valid", [0xA1, 0x01, 0x00, 0x01, 0x01, 0x01, 0xCC, 0xCC],
           "M1 metadata读取：app_valid=过；来源=主区0x0801C000生效")
    # M2 自测向量 = 统一后 wire 序 b1=copy result b2=detail（D5 P0-2）
    dcheck("M2 seq start", [0xA2, 0x00, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC],
           "M2 搬运序列开始")
    dcheck("M2 copy ok", [0xA2, 0x01, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC],
           "M2 搬运结果：成功（Backup→App 已提交）")
    dcheck("M2 not pending", [0xA2, 0x00, 0xFF, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC],
           "M2 搬运结果：无待搬运固件")
    dcheck("M2 copy fail", [0xA2, 0xFF, 0x03, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC],
           "M2 搬运结果：失败 detail=0x03")
    dcheck("M3 backup verify pass", [0xA3, 0x01, 0x00, 0x00, 0xCC, 0xCC, 0xCC, 0xCC],
           "M3 验签：通过 目标=Backup区(源)")
    dcheck("M3 app verify fail ECDSA", [0xA3, 0x00, 0x06, 0x01, 0xCC, 0xCC, 0xCC, 0xCC],
           "M3 验签：失败 关卡6(ECDSA签名失败) 目标=App区")
    dcheck("M3 verify start marker", [0xA3, 0x00, 0xFF, 0x01, 0xCC, 0xCC, 0xCC, 0xCC],
           "M3 验签开始：目标=App区")
    dcheck("M4 app entry", [0xA4, 0x00, 0x41, 0x00, 0x08, 0xCC, 0xCC, 0xCC],
           "M4 跳转目标：0x08004100→App区")
    dcheck("empty frame", [], "无法解码")

    ok_chain = [parse_diag_frame(f) for f in (
        [0xA1, 0x01, 0x00, 0x01, 0x01, 0x01, 0xCC, 0xCC],
        [0xA2, 0x01, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC],
        [0xA3, 0x01, 0x00, 0x00, 0xCC, 0xCC, 0xCC, 0xCC],
        [0xA3, 0x01, 0x00, 0x01, 0xCC, 0xCC, 0xCC, 0xCC],
        [0xA4, 0x00, 0x41, 0x00, 0x08, 0xCC, 0xCC, 0xCC])]
    lines = summarize_chain(ok_chain, (0, "QC_JYF_FW_1.1.1"), (0, "QC_JYF_FW_1.1.2"))
    cases.append(("chain summary single-App",
                  any("实跳=0x08004100(App区)" in l for l in lines)
                  and any("一致（" in l for l in lines), " | ".join(lines)))

    passed = sum(1 for c in cases if c[1])
    print("=== zcanpro_boot_diag_capture single-App decode self-test ===")
    for name, ok, s in cases:
        print("[%s] %s\n        → %s" % ("PASS" if ok else "FAIL", name, s))
    print("=== 自测结果：%s（%d/%d 通过）==="
          % ("全部通过" if passed == len(cases) else "存在失败", passed, len(cases)))
    return passed == len(cases)


if __name__ == "__main__" and zcanpro is None:
    ok = _selftest()
    sys.exit(0 if ok else 1)
