# OTA-ARCH-0920-D3 P0-P2 脚本修复二次审查报告

- **审查对象**：commit `51c9af9`（已推 origin/main，`origin/main == 51c9af93` 实测一致）
- **改动文件**：`python_tools/2.functional test script/zcanpro_read_app_version.py`（+25/−10）
- **基线**：`857af1d`（`git rev-parse 51c9af9^` = `857af1dc…`，与任务所述基线一致）
- **范围核对**：`git diff --name-only 857af1d..51c9af9` 唯一命中即上述文件——零触碰其他对象由构造成立
- **审查时间**：2026-09-21 04:10–04:25（Asia/Shanghai）
- **方式**：只读静态审查（AST 级函数体比对 + py_compile + pyflakes + 全文 grep），未执行脚本、未改仓库任何文件
- **结论**：**PASS_WITH_NOTES**（无必改项；3 条 notes 见风险段）

> 以下行号均指 commit `51c9af9` 版本文件（`git show 51c9af9:<path>` 取证侧）。

---

## 一、P0 重试参数

| 审查点 | 结论 | 证据 |
|---|---|---|
| `range(1,3)→range(1,9)`（8 次重试） | ✅ 确认 | zcanpro_read_app_version.py:**L348** `for i in range(1, 9):`；diff 显示 `-    for i in range(1, 3):` |
| 间隔 `0.15s→0.5s` | ✅ 确认 | **L359** `time.sleep(0.5)`（旧值 0.15 见 diff `-` 行） |
| 日志格式 `唤醒 %d/8` 与循环次数一致 | ✅ 一致 | **L358** `_log("唤醒 %d/8: %s" % (i, e))`；循环上界 9（即 1..8），格式串 `/8` 与实际最大次数吻合 |
| UDS 超时参数是否同步调整 | ✅ 无需调整，确认合理 | `_uds_init()` **L82-89** 经 AST 比对与基线 **SAME**（`response_timeout_ms: 2000` **L85** 未动）。docstring **L344-346** 的覆盖数学自洽：8×2s 应答超时 + 7×0.5s 间隔 ≈ 19.5s ≫ D3 审计（OTA-ARCH-0920-D3-full-audit.md:45,68）给出的最坏 ~5s Boot→App 黑窗（backup_valid=1 时 2.8–4.0s，审计建议按 5s 覆盖、重试 5–8 次——本交付取上限 8 次）。超时不动不构成覆盖缺口 |
| 8 次全失败后的 fallback 路径 | ✅ 正确 | 8 次失败后 `ok=False` 返回（**L361**）→ `run()` **L405-426** 进入诊断分支：重新 `_uds_init()`（**L412**）→ raw 3E/22 21 13 → `_sniff` → 有帧则继续 raw DID 读取，0 帧则输出排查指引——fallback 逻辑为基线既有结构，本次仅改文案与 init 调用，AST 比对确认判定骨架未漂移 |

**P0 小结**：修复参数与 D3 审计建议（重试 5–8 次覆盖 5s 黑窗）对齐，实现与头部变更记录声称一致。

## 二、P1 raw 嗅探路径

| 审查点 | 结论 | 证据 |
|---|---|---|
| `_uds_deinit()` → `_uds_init()` 替换确认 | ✅ 确认 | **L412** `_uds_init()`；diff `-    _uds_deinit()` / `+    _uds_init()`。基线该处 deinit 后嗅探必 0 帧（D2 审计已标注该观测量无诊断价值） |
| `_uds_init()` 调用参数与首次调用一致 | ✅ 一致（同一无参函数） | 首次调用 **L381**、嗅探前调用 **L412**，均为无参调用同一 `_uds_init()`（**L82-89**，AST SAME），参数集恒等 |
| `z_main` finally 的 `_uds_deinit()` 清理未被误改 | ✅ 未误改 | **L470** `finally: _uds_deinit()` 在位；`z_main` 与 `_uds_deinit` 函数体 AST 比对均 **SAME** |
| `_sniff()` docstring 补充 init 前提说明 | ✅ 已补充 | **L310-312**：新增「前提：UDS 处于 init 状态——ZLG deinit 后 receive 恒返回 (1,[])，嗅探必为 0 帧」；`_sniff` 函数体（剥 docstring 后）AST **SAME**，仅文档变更 |
| `_uds_init()` 失败的兜底处理 | ✅ 可接受（响亮失败，见 note-2） | **L412** 无局部 try/except；若 zcanpro.uds_init 抛异常，上行 `run()` 不捕获，穿透至 `z_main` **L467-468** `except → _log("失败: ...")`，再 `finally L470` `_uds_deinit()`（内部 try/except 包裹，二次异常不逃逸）。失败模式＝响亮拒绝并留日志，无静默误判 |

**P1 小结**：修复方向正确——新路径上 UDS 从未 deinit（L381 init 后直达 L412），receive 通路本应已开，补 init 是双保险；行为细节见 note-2。

## 三、P2 文案更新

| 审查点 | 结论 | 证据 |
|---|---|---|
| `Slot A\|28KB Boot\|slotB\|slot_a\|slotA` 零残留 | ✅ 零残留 | `git show 51c9af9:<file> \| grep -nE "Slot A\|28KB Boot\|slotB\|slot_b\|slot_a\|slotA\|SlotB"` → **exit 1，零命中**（全文件扫描，非仅 diff 内） |
| 新文案与分区表一致 | ✅ 一致 | 脚本 **L422/L426-427** 与头部 **L17**：`Boot(16KB)+App(48KB)+Backup(48KB)`、"单 App 架构，无 A/B 槽位" ↔ docs/2. Flash 分配方案.md「Flash 布局总览」表：Bootloader `0x08000000-0x08003FFF` 16KB / App 区 `0x08004000-0x0800FFFF` 48KB / Backup `0x08010000-0x0801BFFF` 48KB，文档头部明示"A/B 双槽 ping-pong 机制已永久取消"。数值与架构口径逐项吻合 |
| 头部 docstring 变更记录准确 | ✅ 准确 | **L19-22** 变更记录三条：P0（2→8 次、0.15→0.5s）对 L348/L359 ✓；P1（嗅探前重 init）对 L412 ✓；P2（旧架构文案更新）对 L422/L426 ✓。**L12-13** 流程第 1 条同步改为"最多连发 8 次" ✓ |

## 四、横向检查

| 审查点 | 结论 | 证据 |
|---|---|---|
| UDS 请求格式/CAN ID/DID/解析逻辑零改动 | ✅ 代码级零改动 | AST 逐函数比对（新旧两版各剥 docstring 后 `ast.dump` 全量对照）：`uds_req`、`isotp_raw_request`、`parse_did_string`、`read_did_string`、`_uds_init`、`_uds_deinit`、`_make_frame`、`can_send`、`_parse_one_frame`、`_unwrap_receive`、`can_recv`、`_assemble`、`z_main`、`z_notify`、`_hex`、`_pad8`、`_log`、`_as_int` 全部 **SAME**；模块常量 `UDS_REQ_ID`/`UDS_RESP_ID`/`SID_*`/`DID_LIST` 全部 **SAME**。CHANGED 仅 `wake_mcu`（P0 本体）与 `run`（P1 本体+P2 文案） |
| py_compile + pyflakes | ✅ 双通过 | `PYTHONPYCACHEPREFIX=/tmp/... python3 -m py_compile`（pyc 重定向到 /tmp，未向仓库写 `__pycache__`）→ OK；`python3 -m pyflakes` → 零告警 OK |
| 无新增/未使用 import | ✅ 确认 | 全文仅 `import time`（L27）+ try/except `import zcanpro`（L29-32），与基线相同；diff 无 import hunk；pyflakes 零告警佐证 |
| 改动未波及其他功能模块 | ✅ 确认 | name-only diff 唯一文件；函数级 AST 比对仅 2 函数 CHANGED 且均为交付声称范围；DID 读取主流程、汇总输出、异常记录逻辑均未触碰 |
| 自检声明证伪（commit message 三项声称） | ✅ 均成立 | "重试 2→8+0.5s"→L348/L359 ✓；"_sniff 前重 init"→L412 ✓；"旧架构文案更新"→grep 零残留+L422/426 ✓。注意：本次**无运行期冒烟证据**（交付 commit 不含执行日志），上述结论均为**实现核验（代码级）**，不表述为"行为正确" |

**跨脚本复制代码核查**：不适用——本交付无新增函数、无新增模块级名（AST ADDED 项为零），不存在"复制带走引用不带走定义"暴露面。**密码学 oracle 验证（步骤 4a）**：不适用——改动不含签名/验签/密钥解析逻辑；docstring 中引用的 ECDSA 验签×2 仅为黑窗耗时说明文字，非被审代码。

---

## 五、风险段 / Notes（非必改）

1. **note-1｜"deinit 后 receive 恒返回 (1,[])" 为继承性声称，库语义未独立验证**：该断言在 `_sniff` docstring（L312）与 run() 注释（L408-410）中作为修复依据出现。其来源是脚本基线既有工程经验注释（857af1d 版 L368「不 init 时 receive 恒为 (1,[])"，V1.0.0 实测口径）+ D3 审计结论（OTA-ARCH-0920-D3-full-audit.md:17）；但 D2 审计自身（OTA-ARCH-0920-D2-log-audit.md:61）标注该库行为"未验证"。定性：文档级声称，未在本仓库内取得 ZLG 库二进制级证据。**不影响 verdict**——即便库语义相反（deinit 后 receive 仍可用），新代码（不 deinit + 补 init）只会让嗅探更可靠，修复方向 fail-safe；且新路径从未调用 deinit，声称与代码行为不构成运行逻辑依赖。
2. **note-2｜L412 对未 deinit 状态二次 `_uds_init()` 的 ZLG 库容错性未验证**：新路径时序为 init(L381)→wake 8 次→**再次 init(L412)**。ZLG 库对重复 uds_init 的行为（幂等/报错/句柄泄漏）无仓库内证据。失败模式评估：若库拒绝重复 init 并抛异常 → `z_main` L467-468 响亮捕获并记日志、L470 清理，脚本以明确失败退出而非静默误判；若幂等/无害 → 嗅探通路正常打开。两分支均无"误报设备状态"的静默风险。建议后续实测一次失败路径（设备不在线时跑脚本）确认日志形态。
3. **note-3｜wake_mcu 非异常非 7E 应答路径无日志无间隔（INFO）**：`uds_req` 返回非空但 `rx[0] != 0x7E` 时（L354-356 条件不成立且无 else），循环直接进入下一次尝试——不打"唤醒 %d/8"日志、不 sleep。非忙等（每次 uds_req 至少阻塞至库返回），且 UDS 正响应字节约束下该分支实际极难出现；判定逻辑与基线一致（基线同样结构）。INFO，不构成缺陷。

---

## 六、重测适用性结论

| 组件 | 结论 | 三要素 |
|---|---|---|
| 版本读取主流程（UDS 在线路径，L383-406） | **放行重测** | ① 关键路径零改动：`uds_req`/`parse_did_string`/`DID_LIST`/CAN ID AST 代码级 SAME；② fail-safe：本路径未受 P0-P2 改动触及，失败模式不变；③ 业务门禁：DID 期望值常量未动，与固件编译常量对应关系不变 |
| wake_mcu 唤醒路径（P0） | **放行重测** | ① 改动仅重试次数/间隔/日志，应答判定（`rx[0]==0x7E`）SAME；② fail-safe：8 次全失败响亮落入诊断分支，不误报在线；③ 窗口 19.5s 覆盖审计最坏 ~5s 黑窗，参数与审计建议区间（5–8 次）吻合 |
| raw 嗅探诊断路径（P1） | **放行重测（条件受限：note-2）** | ① 嗅探判定逻辑 `_sniff` 代码级 SAME，仅前置 init 调用改变；② 失败模式为响亮拒绝（日志"失败: ..."），不产生假阴性诊断结论；③ 受限场景：若 ZLG 库拒绝重复 init，诊断分支无法执行——补救：实测一次离线场景确认，或后续改为 `_uds_deinit()` 紧跟 `_uds_init()` 的成对调用（本次审查只读，不改码） |
| 文案/排查指引（P2） | **放行** | 纯日志与注释文案，与分区文档逐项一致，零运行逻辑影响 |

---

## 七、JSON 摘要

```json
{
  "summary": "commit 51c9af9 对 zcanpro_read_app_version.py 的 P0-P2 修复全部核实成立：P0 重试 range(1,9)+0.5s 与日志格式/头部变更记录一致，19.5s 覆盖窗口满足 D3 审计最坏 ~5s 黑窗，UDS 超时 2000ms 无需联动调整；P1 嗅探前 _uds_init() 替换确认（L412），z_main finally 清理未误改，_sniff docstring 已补 init 前提；P2 旧架构文案全文件 grep 零残留，新文案 Boot(16KB)+App(48KB)+Backup(48KB) 与 docs/2. Flash 分配方案.md 逐项一致。AST 函数级比对证明 UDS/CAN ID/DID/版本解析路径代码级零改动，py_compile+pyflakes 双通过，无新增 import，改动零外溢。3 条非阻塞 notes：ZLG 库 deinit 语义与二次 init 容错性均未独立验证（失败模式均为响亮拒绝，无静默误判风险）、wake_mcu 非 7E 应答分支无日志（INFO）。无必改项，可放行重测（raw 嗅探诊断路径附条件）。",
  "verdict": "PASS_WITH_NOTES",
  "confidence": 90,
  "issues": [
    {"severity": "info", "file": "python_tools/2.functional test script/zcanpro_read_app_version.py", "line": 312, "desc": "「deinit 后 receive 恒返回 (1,[])」为继承性库语义声称（基线注释+D3 审计），D2 审计标注该行为未验证；文档级，不影响运行逻辑与修复方向", "fix": "后续实测 ZLG 库 deinit 后 receive 行为以夯实证据链；无需改码"},
    {"severity": "low", "file": "python_tools/2.functional test script/zcanpro_read_app_version.py", "line": 412, "desc": "对未 deinit 状态二次调用 _uds_init()，ZLG 库重复 init 容错性未验证；失败时由 z_main L467-468 响亮捕获，无静默误判风险", "fix": "实测一次设备离线场景确认失败日志形态；或后续改为 deinit+init 成对调用"},
    {"severity": "info", "file": "python_tools/2.functional test script/zcanpro_read_app_version.py", "line": 354, "desc": "wake_mcu 中 uds_req 返回非 7E 正响应时循环无日志无间隔直接重试（与基线同构，实际极难触发，非忙等）", "fix": "INFO，可不处理；如需完备可补 else 分支日志"}
  ]
}
```

**verdict：PASS_WITH_NOTES** — 无 blocking_issues，无必改条目。
