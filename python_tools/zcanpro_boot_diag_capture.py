# -*- coding: utf-8 -*-
"""
ZCANPRO 扩展脚本 — Boot 诊断帧抓取（开机决策链 M1~M4）

配套 Boot 诊断标记版（commit 60d1137）：复位设备 → 抓取开机决策链四点
CAN 标记帧（ID=0x18FF480D）→ 逐帧人话解码 → 决策链串联摘要。

用法（ZCANPRO → 高级功能 → 扩展脚本 → 打开本文件 → 运行）：
  前提：设备 Boot 已烧录 60d1137 诊断标记版构建（普通版 Boot 不发
  诊断帧，零帧提示会引导排查）；CAN 通道 250kbps Classical CAN 扩展帧。
  流程：①读基线（22 2113 + 22 F195）→ ②raw 监听先行后发复位 11 01
  （非 suppress，≤3 次重试，等 51 01）→ ③捕获窗口 ≥30s（常量可调）
  ——窗口内打印收到的【每一帧】原始数据（含非诊断帧，无需上位机
  过滤），0x18FF480D 额外人话解码 → ④APP 回来后再读 22 2113 + 22
  F195 → ⑤窗口结束打印总线活动统计（总帧数/按 ID 分布/诊断帧数）
  +零帧二分提示（区分『总线死』与『有流量无诊断帧』）+决策链摘要
  +一致性对照（M4 实跳 vs 2113 实读）。

解码表与 60d1137 终审规格逐字节一致（boot_safe_mode.h BOOT_DIAG 注释）：
  M1 0xA1: [active_slot][meta_src][magic_ok][ver_ok][crc_ok]
  M2 0xA2: [slot][ret]
  M3 0xA3: [pass][fail_step][slot]
  M4 0xA4: [app_addr LE 4B]
  帧格式：DLC=8，[标记 0xA1~0xA4][数据...][0xCC 填充]。

本脚本为独立诊断工具：不签名、不写设备、无私钥/固件路径引用；
升级脚本 zcanpro_ext_ota_auto.py 零关系零触碰。
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
RESET_RETRIES = 3                 # 11 01 重试次数（模式照升级脚本）
RESET_WAIT_S = 2.0                # 每次复位命令等 51 01 的时长
POLL_INTERVAL_S = 0.01

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


def _slot_name(v):
    return {0: "A", 1: "B"}.get(int(v) & 0xFF, "未知(0x%02X)" % (int(v) & 0xFF))


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
        return "A槽"
    if a == 0x08010100:
        return "B槽"
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
        return {"marker": 0xA1, "active_slot": b[1], "meta_src": b[2],
                "magic_ok": b[3], "ver_ok": b[4], "crc_ok": b[5]}
    if m == 0xA2:
        if len(b) < 3:
            return {"marker": 0xA2, "raw": b, "error": "M2 帧长度不足3"}
        return {"marker": 0xA2, "slot": b[1], "ret": b[2]}
    if m == 0xA3:
        if len(b) < 4:
            return {"marker": 0xA3, "raw": b, "error": "M3 帧长度不足4"}
        return {"marker": 0xA3, "passed": b[1], "fail_step": b[2], "slot": b[3]}
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
        return ("[诊断] M1 metadata读取：active=%s；来源=%s；magic=%s 版本=%s CRC=%s"
                % (_slot_name(p["active_slot"]),
                   META_SRC.get(p["meta_src"], "未知(0x%02X)" % p["meta_src"]),
                   _bool1(p["magic_ok"]), _bool1(p["ver_ok"]), _bool1(p["crc_ok"])))
    if m == 0xA2:
        if p["ret"] == 0:
            return "[诊断] M2 选槽结果：选中=%s" % _slot_name(p["slot"])
        return ("[诊断] M2 选槽结果：失败(ret=0x%02X→safe mode路径)；"
                "metadata槽字段原值=%s" % (p["ret"], _slot_name(p["slot"])))
    if m == 0xA3:
        if p["passed"]:
            return "[诊断] M3 验签：通过 槽=%s" % _slot_name(p["slot"])
        return ("[诊断] M3 验签：失败 关卡%d(%s) 槽=%s"
                % (p["fail_step"],
                   FAIL_STEP.get(p["fail_step"], "未知关卡"),
                   _slot_name(p["slot"])))
    if m == 0xA4:
        return "[诊断] M4 跳转目标：0x%08X→%s" % (p["addr"], _addr_target(p["addr"]))
    return "[诊断] 未知标记 0x%02X" % m


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
    """总帧数=0 提示：总线完全无流量 → 物理层排查。"""
    return ("[诊断] 总线完全无任何帧（连非诊断流量都没有）→ 按物理层排查："
            "设备上电/CAN 接线/CAN 通道参数（250kbps 扩展帧）；若确认已烧 "
            "60d1137 诊断版 Boot 且设备已上电仍零帧，回报 agent:main。"
            "注意：零帧≠决策证据——不能据此判定 Boot 决策行为。")


def traffic_no_diag_hint(st):
    """总帧数>0 且诊断帧=0 提示：有流量无诊断帧 → Boot 版本/开机事件二分。"""
    parts = ["ID=0x%08X ×%d" % (k, v) for k, v in sorted(st["ids"].items())]
    return ("[诊断] 总线有流量但无 0x18FF480D 诊断帧（总帧数=%d，ID 分布：%s）"
            "→ 设备 Boot 可能非诊断版（未烧 60d1137 构建）或本窗口内未发生"
            "开机事件——断电重试一次再看（断电重试期间本窗口继续监听并"
            "打印全部帧，无需任何上位机操作）。注意：无诊断帧≠决策证据。"
            % (st["total"], "，".join(parts) if parts else "无"))


def summarize_chain(parsed_frames, pre, post):
    """决策链串联摘要。parsed_frames=parse_diag_frame 结果序列；
    pre/post=(slot, ver) 或 (None, None)。返回人话行列表。纯函数。"""
    m1 = next((p for p in parsed_frames if p and p.get("marker") == 0xA1 and "error" not in p), None)
    m2 = next((p for p in parsed_frames if p and p.get("marker") == 0xA2 and "error" not in p), None)
    m3s = [p for p in parsed_frames if p and p.get("marker") == 0xA3 and "error" not in p]
    m4 = next((p for p in parsed_frames if p and p.get("marker") == 0xA4 and "error" not in p), None)

    if m1:
        s1 = "读记录=active%s(%s,校验magic:%s/ver:%s/crc:%s)" % (
            _slot_name(m1["active_slot"]),
            META_SRC.get(m1["meta_src"], "src=0x%02X" % m1["meta_src"]),
            _bool1(m1["magic_ok"]), _bool1(m1["ver_ok"]), _bool1(m1["crc_ok"]))
    else:
        s1 = "读记录=无M1帧"
    if m2:
        s2 = "选槽=%s" % (_slot_name(m2["slot"]) if m2["ret"] == 0
                          else "失败(ret=0x%02X,槽字段=%s)" % (m2["ret"], _slot_name(m2["slot"])))
    else:
        s2 = "选槽=无M2帧"
    if m3s:
        parts = []
        for p in m3s:
            if p["passed"]:
                parts.append("%s槽通过" % _slot_name(p["slot"]))
            else:
                parts.append("%s槽失败(关卡%d:%s)" % (
                    _slot_name(p["slot"]), p["fail_step"],
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


# ======== ZCANPRO 通道/收发（模式参照仓内既有脚本，零依赖它们） ========
def _recv_frames_min(bus_id):
    """raw 收帧一轮：兼容 (status,[frames]) 返回与多态帧对象。"""
    try:
        raw = zcanpro.receive(bus_id)
    except TypeError:
        try:
            raw = zcanpro.receive()
        except Exception:
            return
    except Exception:
        return
    frames = raw
    if isinstance(raw, tuple) and len(raw) == 2 and isinstance(raw[1], (list, tuple)):
        frames = raw[1]
    if frames is None:
        return
    if not isinstance(frames, (list, tuple)):
        frames = [frames]
    for f in frames:
        cid = dat = None
        if isinstance(f, dict):
            cid = f.get("can_id", f.get("id", f.get("ID")))
            dat = f.get("data", f.get("Data"))
        elif isinstance(f, (tuple, list)) and len(f) >= 2:
            cid, dat = f[0], f[1]
        else:
            cid = getattr(f, "can_id", getattr(f, "id", None))
            dat = getattr(f, "data", None)
        if cid is None or dat is None:
            continue
        yield (int(cid) & 0xFFFFFFFF, _to_list(dat))


def _raw_send_min(bus_id, can_id, data):
    """raw 扩展帧发送（bit31 置位+is_extend 标志；transmit/send 双兼容）。"""
    cid = (int(can_id) & 0x1FFFFFFF) | EXT_FLAG
    frame = {"can_id": cid, "id": cid, "data": _to_list(data),
             "dlc": 8, "len": 8, "is_extend": 1, "is_extended": 1}
    for name in ("transmit", "send"):
        fn = getattr(zcanpro, name, None)
        if fn is None:
            continue
        try:
            fn(bus_id, frame)
            return True
        except Exception:
            try:
                fn(frame)
                return True
            except Exception:
                continue
    return False


def _uds_init_min():
    cfg = {"baudrate": 250000, "src_addr": UDS_REQ_ID, "dst_addr": UDS_RESP_ID}
    try:
        zcanpro.uds_init(cfg)
    except Exception:
        zcanpro.uds_init()


def _is_51_01(dat):
    if not dat or len(dat) < 2:
        return False
    if len(dat) >= 3 and dat[0] == 0x02 and dat[1] == 0x51 and dat[2] == 0x01:
        return True
    return dat[0] == 0x51 and dat[1] == 0x01


def read_slot_ver(bus_id, label):
    """读 22 2113 + 22 F195 → (slot, ver)；单项失败不拦截流程。"""
    slot = None
    ver = None
    try:
        resp = zcanpro.uds_request(bus_id, {
            "src_addr": UDS_REQ_ID, "dst_addr": UDS_RESP_ID,
            "suppress_response": 0, "sid": 0x22, "data": [0x21, 0x13]})
        data = _to_list((resp or {}).get("data") or [])
        if len(data) >= 4 and data[0] == 0x62 and data[1] == 0x21 and data[2] == 0x13:
            slot = data[3]
            _log("[%s] 0x2113=%s" % (label, _slot_name(slot)))
        else:
            _log("[%s] 0x2113 应答异常: %s" % (label, _hex(data[:8])))
    except Exception as e:
        _log("[%s] 0x2113 读取失败: %s" % (label, e))
    try:
        resp = zcanpro.uds_request(bus_id, {
            "src_addr": UDS_REQ_ID, "dst_addr": UDS_RESP_ID,
            "suppress_response": 0, "sid": 0x22, "data": [0xF1, 0x95]})
        data = _to_list((resp or {}).get("data") or [])
        if len(data) >= 4 and data[0] == 0x62 and data[1] == 0xF1:
            raw = bytes(bytearray(data[3:])).split(b"\x00")[0]
            ver = raw.decode("ascii", "ignore").strip() or None
            _log("[%s] 0xF195=%s" % (label, ver or "未读到"))
        else:
            _log("[%s] 0xF195 应答异常: %s" % (label, _hex(data[:8])))
    except Exception as e:
        _log("[%s] 0xF195 读取失败: %s" % (label, e))
    return slot, ver


# ======== 主流程 ========
def z_main():
    global stopTask
    stopTask = False
    if zcanpro is None:
        _log("本脚本需在 ZCANPRO 扩展脚本引擎内运行（宿主机自测请直接 python3 运行）")
        return
    _log("======== Boot 诊断帧抓取（CAN ID 0x18FF480D，配套 60d1137）========")
    buses = zcanpro.get_buses()
    _log("总线 " + str(buses))
    if not buses:
        _log("请先打开 CAN 通道 250kbps Classical CAN 扩展帧")
        return
    bus_id = buses[0]["busID"]

    # 1) UDS 模式读基线
    _uds_init_min()
    pre = read_slot_ver(bus_id, "基线-复位前")
    try:
        zcanpro.uds_deinit()
    except Exception as e:
        _log("UDS 通道释放失败（继续）: %s" % e)

    events = []          # (t_rel, cid, data)——窗口内全部帧，统计唯一真相源
    parsed = []          # parse_diag_frame 结果序列

    def _collect(t0):
        """收帧一轮：打印每一帧原始数据（诊断帧额外人话解码）；
        返回本轮 (cid, dat) 列表供复位 51 01 检测。"""
        seen = []
        for cid, dat in _recv_frames_min(bus_id):
            t_rel = time.time() - t0
            events.append((t_rel, cid, dat))
            seen.append((cid, dat))
            _log("[Rx] T+%7.3fs ID=0x%08X DLC=%d data=%s"
                 % (t_rel, cid & 0xFFFFFFFF, len(dat), _hex(dat)))
            if (cid & 0x1FFFFFFF) == DIAG_CAN_ID:
                parsed.append(parse_diag_frame(dat))
                _log(decode_diag_frame(dat))
        return seen

    # 2) 监听先行（0.5s 预热，防漏帧），再发复位
    _log("[诊断] raw 监听已建立（0.5s 预热后进入复位流程）")
    t_start = time.time()
    t_warm = time.time() + 0.5
    while time.time() < t_warm and not stopTask:
        _collect(t_start)
        time.sleep(POLL_INTERVAL_S)

    reset_ok = False
    for attempt in range(1, RESET_RETRIES + 1):
        if stopTask:
            raise RuntimeError("用户停止脚本")
        if not _raw_send_min(bus_id, UDS_REQ_ID,
                             [0x02, 0x11, 0x01, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC]):
            _log("[诊断] raw 发送 API 不可用（transmit/send 均失败），跳过第 %d 次"
                 % attempt)
            continue
        _log("[诊断] 复位 11 01（非 suppress，raw ISO-TP SF）第 %d/%d 次已发送"
             % (attempt, RESET_RETRIES))
        t_wait = time.time() + RESET_WAIT_S
        while time.time() < t_wait and not stopTask:
            for cid, dat in _collect(t_start):
                if (cid & 0x1FFFFFFF) == UDS_RESP_ID and _is_51_01(dat):
                    reset_ok = True
            if reset_ok:
                break
            time.sleep(POLL_INTERVAL_S)
        if reset_ok:
            _log("[诊断] 已收到 51 01 正响应：设备确认复位，进入 Boot 决策链观测")
            break
    if not reset_ok:
        _log("[诊断] !!!!! 11 01 三次均未收到 51 01 应答 !!!!! 设备可能无响应"
             "或链路异常；可手动断电重试（POR 同样触发 Boot 决策链）——"
             "断电重试期间本窗口继续监听并打印收到的全部帧（含非诊断帧），"
             "无需任何上位机操作；窗口结束的总线活动统计会区分"
             "『总线死』与『有流量无诊断帧』")

    # 3) 捕获窗口：复位命令后 ≥CAPTURE_WINDOW_S
    t_win_end = t_start + CAPTURE_WINDOW_S
    while time.time() < t_win_end and not stopTask:
        _collect(t_start)
        time.sleep(POLL_INTERVAL_S)
    _log("[诊断] 捕获窗口结束（%.1fs）" % CAPTURE_WINDOW_S)
    st = summarize_bus(events)
    _log(format_bus_stats(st))
    if st["diag"] == 0:
        if st["total"] == 0:
            _log(bus_silent_hint())
        else:
            _log(traffic_no_diag_hint(st))

    # 4) APP 回来后复读 DID
    _uds_init_min()
    post = read_slot_ver(bus_id, "基线-复位后")

    # 5) 决策链摘要
    _log("======== 决策链摘要 ========")
    for line in summarize_chain(parsed, pre, post):
        _log(line)
    if stopTask:
        _log("[诊断] （用户中途停止，摘要基于已捕获帧）")


# ======== 宿主机合成帧自测（无 zcanpro 时运行；铁律：交付前自测） ========
def _selftest():
    cases = []

    def check(name, frame, expect_sub):
        s = decode_diag_frame(frame)
        cases.append((name, expect_sub in s, s))

    check("M1 正常(B槽/主区/全过)",
          [0xA1, 0x01, 0x00, 0x01, 0x01, 0x01, 0xCC, 0xCC],
          "M1 metadata读取：active=B；来源=主区0x0801C000生效；magic=过 版本=过 CRC=过")
    check("M1 defaults兜底+全不过",
          [0xA1, 0x00, 0x02, 0x00, 0x00, 0x00, 0xCC, 0xCC],
          "active=A；来源=defaults兜底(双区无效)；magic=不过 版本=不过 CRC=不过")
    check("M1 异常值(active=0xFF/src=0xFF)",
          [0xA1, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xCC, 0xCC],
          "active=未知(0xFF)；来源=未记录(init未执行)")
    check("M2 成功选B", [0xA2, 0x01, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC],
          "M2 选槽结果：选中=B")
    check("M2 失败路径(槽字段原值=0x0A)",
          [0xA2, 0x0A, 0xFF, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC],
          "失败(ret=0xFF→safe mode路径)；metadata槽字段原值=未知(0x0A)")
    check("M3 失败关卡6(ECDSA) B槽",
          [0xA3, 0x00, 0x06, 0x01, 0xCC, 0xCC, 0xCC, 0xCC],
          "M3 验签：失败 关卡6(ECDSA签名失败) 槽=B")
    check("M3 通过 A槽", [0xA3, 0x01, 0x00, 0x00, 0xCC, 0xCC, 0xCC, 0xCC],
          "M3 验签：通过 槽=A")
    check("M3 异常值(未知关卡9)",
          [0xA3, 0x00, 0x09, 0x00, 0xCC, 0xCC, 0xCC, 0xCC],
          "关卡9(未知关卡) 槽=A")
    check("M4 跳B(0x08010100 LE)",
          [0xA4, 0x00, 0x01, 0x01, 0x08, 0xCC, 0xCC, 0xCC],
          "M4 跳转目标：0x08010100→B槽")
    check("M4 跳A(0x08004100 LE)",
          [0xA4, 0x00, 0x41, 0x00, 0x08, 0xCC, 0xCC, 0xCC],
          "M4 跳转目标：0x08004100→A槽")
    check("M4 异常值(0x00000000)",
          [0xA4, 0x00, 0x00, 0x00, 0x00, 0xCC, 0xCC, 0xCC],
          "0x00000000→非标准目标")
    check("短帧(M1不足6字节)", [0xA1, 0x01], "M1 帧长度不足6")
    check("空帧", [], "无法解码")
    check("未知标记0xA5", [0xA5, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00],
          "无法解码")

    # ---- 合成流三场景（铁律：多 ID 含诊断/多 ID 无诊断/全静默）----
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

    ok_chain = [parse_diag_frame(f) for f in (
        [0xA1, 0x00, 0x00, 0x01, 0x01, 0x01, 0xCC, 0xCC],
        [0xA2, 0x01, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC],
        [0xA3, 0x01, 0x00, 0x01, 0xCC, 0xCC, 0xCC, 0xCC],
        [0xA4, 0x00, 0x01, 0x01, 0x08, 0xCC, 0xCC, 0xCC])]
    lines = summarize_chain(ok_chain, (0, "QC_JYF_FW_1.1.1"), (1, "QC_JYF_FW_1.1.2"))
    cases.append(("摘要：正常链(B跳B报B)一致",
                  any("一致（" in l for l in lines) and any("实跳=0x08010100" in l for l in lines),
                  " | ".join(lines)))
    lines2 = summarize_chain(ok_chain, (0, "QC_JYF_FW_1.1.1"), (0, "QC_JYF_FW_1.1.1"))
    cases.append(("摘要：矛盾链(B跳B但自报A)不一致告警",
                  any("不一致" in l for l in lines2),
                  " | ".join(lines2)))
    lines3 = summarize_chain([], None, None)
    cases.append(("摘要：零帧信息不足分支",
                  any("信息不足" in l for l in lines3) and any("无M1帧" in l for l in lines3),
                  " | ".join(lines3)))

    passed = sum(1 for c in cases if c[1])
    print("=== zcanpro_boot_diag_capture 合成流自测 ===")
    for name, ok, s in cases:
        print("[%s] %s\n        → %s" % ("PASS" if ok else "FAIL", name, s))
    print("=== 自测结果：%s（%d/%d 通过）==="
          % ("全部通过" if passed == len(cases) else "存在失败", passed, len(cases)))
    return passed == len(cases)


if __name__ == "__main__" and zcanpro is None:
    ok = _selftest()
    sys.exit(0 if ok else 1)
