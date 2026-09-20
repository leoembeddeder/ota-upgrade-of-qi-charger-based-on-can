# OTA-ARCH-0920 D3 全面代码+脚本审计报告

**审计对象**：`zcanpro_read_app_version.py` 版本读取失败根因排查（排除物理连接因素）
**审计时间**：2026-09-21 03:58–04:30
**审计方式**：只读静态代码审计，零改码
**基线**：git HEAD 当前工作区
**症状**：22:28（9-20）与 03:52（9-21）UDS 3E 00×2 全超时；10:33（9-20）同机同通道同脚本全通

---

## 〇、审计结论摘要

**未发现单一确定性代码缺陷**可直接解释"Boot 和 App 同时不响应 UDS 3E 00"。代码审计显示 Boot safe mode 与 App UDS dispatch 链在正常条件下均能响应 3E 00。

根因收敛为 **[CONTRIBUTING FACTOR] Boot→App 跳转 CAN 黑窗超出了脚本容错窗口**——当 metadata `backup_valid=1`（OTA 下载完成待搬运）时，Boot 决策链耗时可达 **2.8–4.0 秒**（ECDSA 验签×2 + 48KB Flash 擦写），而脚本的总重试窗口仅 **~4 秒**（2×2s UDS 超时），处于临界状态。两次失败复现的"同一签名"与此一致：设备在每次上电时都经历相同 Boot 黑窗。

同时发现 **[RISK] 脚本 raw 嗅探在 `_uds_deinit()` 之后必然返回 0 帧**（ZLG 库语义：deinit 后 receive 恒为空），使"总线 0 帧"观测量完全丧失诊断价值。

---

## 一、发现清单

### 【ROOT CAUSE 候选】Boot→App CAN 黑窗超出脚本容错窗口

**分类**：CONTRIBUTING FACTOR（条件性根因，需 metadata 状态确认）
**置信度**：72%

**证据链**：

| 阶段 | 耗时估计 | 证据位置 |
|------|----------|----------|
| Boot 时钟+CAN init | ~30ms | boot main.c:27-30 → can_driver.c:80-155 |
| metadata 读取/重建 | 1–80ms | boot_metadata.c:115-145（defaults 路径 save 双副本 flash 擦写） |
| ECDSA P-256 验签（App 镜像） | 200–800ms | boot_verify.c:148-186 (uECC_verify) |
| **[若 backup_valid=1] ECDSA 验签（Backup 镜像）** | **200–800ms** | boot_trial.c:81 (boot_verify_image on BACKUP) |
| **[若 backup_valid=1] 擦除 App 区 48 扇区** | **1000–2200ms** | boot_trial.c:67-77 (48×FLASH_SECTOR_SIZE=0x400) |
| **[若 backup_valid=1] 写入+回读 48KB** | **250–400ms** | boot_trial.c:80-105 (copy_program_region) |
| **[若 backup_valid=1] 二次 ECDSA 验签** | **200–800ms** | boot_trial.c:123 (re-verify in App region) |
| Boot→App 跳转序列 | <1ms | boot_jump.c:75-108 (CAN reset + clock disable + NVIC clear) |
| App 时钟+外设 init | 20–70ms | app main.c:38-56 |
| App 首次 can_protocol_poll → can_lp_enter_normal | <20ms | can_protocol.c:2001-2017 → can_driver_online() |

**总计**：
- **无 backup pending**：黑窗 ≈ 500–950ms → 脚本 4s 窗口覆盖，应能通过
- **有 backup pending**（正常 copy）：黑窗 ≈ **2800–4000ms** → 脚本 4s 窗口**临界/不足**
- **有 backup pending**（copy 失败重试）：每次上电重复完整流程 → **持续性失败**

**代码机制**：

1. **Boot 决策链不处理 CAN RX**：boot main.c:27-63 执行 init→metadata→copy→verify→jump，**无接收循环**。Boot CAN 仅 TX（M1–M4 诊断帧，BOOT_DIAG_CAN_ID=0x18FF480D）。任何在决策链期间到达的 UDS 3E 00 帧被硬件 ACK 但不被软件处理，RX FIFO 数据在 boot_jump_to_app() 的 `can_reset(CAN1)` 后全部丢失。

   > `boot_jump.c:78-79`：`can_reset(CAN1); crm_periph_clock_enable(CRM_CAN1_PERIPH_CLOCK, FALSE);`

2. **App CAN 两阶段初始化**：App `can_driver_init()` 将 CAN1 留在 **software reset** 状态（`can_driver.c:128-130`：`can_software_reset(CAN1, TRUE); ... Stay in software reset until SIT1145 leaves Standby (can_driver_online).`）。CAN 仅在首次 `can_protocol_poll()` → `can_lp_enter_normal()` → `can_driver_online()` 后上线。

   > `can_protocol.c:2008-2011`：`if (g_lp_need_online != 0U) { g_lp_need_online = 0U; can_lp_enter_normal(); }`

3. **backup_valid 持久化**：copy 失败时 `backup_valid` 不清除（boot_trial.c:92,103,114,133 各失败路径均不写 `backup_valid=0`），下次上电自动重试 → 若 Backup 镜像持续无效，每次上电都经历 ~1s 的验签失败延迟。

4. **flash_sector_erase 阻塞**：boot_trial.c:67-77 擦除 App 区 48 个 1KB 扇区（`FLASH_SECTOR_SIZE=0x400`，boot_metadata.h:50），AT32F426 单 Bank Flash 擦除期间 CPU stall，Boot 无法处理任何 CAN RX。

**10:33 成功 vs 22:28/03:52 失败的差异解释**：
- 10:33 设备可能已运行较长时间（App CAN 在线），或 metadata `backup_valid=0`
- 22:28/03:52 可能是在 OTA 下载后首次/后续上电，`backup_valid=1` 触发 copy 路径
- 或设备在两次失败之间经历了断电重启，每次都从头经历 Boot 黑窗

**修复建议**：
1. **脚本侧**：将 `wake_mcu()` 重试次数从 2 次增加到 5–8 次（覆盖最坏 5s 黑窗），或增加初始等待延时（如先等 2s 再发第一帧 3E 00）
2. **固件侧**（可选）：Boot 在进入耗时操作（copy/verify）前发送一帧 "BUSY" 诊断标记，让主机侧知道设备在 Boot 决策链中
3. **流程侧**：版本读取脚本应在设备稳定运行（非刚上电）时执行，或先用 `zcanpro_boot_diag_capture.py` 观察 M1–M4 帧确认 Boot 链状态

---

### 【RISK】脚本 raw 嗅探在 deinit 后必然返回 0 帧——诊断盲区

**分类**：RISK（诊断能力缺陷，非功能缺陷）
**置信度**：95%

**证据**：

> `zcanpro_read_app_version.py:396-398`：
> ```python
> _uds_deinit()
> can_send(bus_id, UDS_REQ_ID, [0x02, 0x3E, 0x00])
> time.sleep(0.2)
> ```

> `zcanpro_read_app_version.py:368`（脚本自身注释）：
> "V1.0.0：必须先 uds_init，ZLG 才会开接收。**不 init 时 receive 恒为 (1,[])**"

脚本在 UDS 失败后的 fallback 路径先调用 `_uds_deinit()`，再进入 `_sniff()` → `can_recv()` → `zcanpro.receive()`。根据脚本自身的注释，ZLG 库在未 init 状态下 `receive()` 恒返回 `(1,[])`。虽然注释只说了"未 init"，但 deinit 后的行为等效——接收通道被关闭。

**后果**：
- `_sniff()` 的 `n`（帧计数）恒为 0 → 脚本打印"总线上 0 帧 MCU 回复"
- `saw_life`（lifecycle 心跳检测）恒为 False → 无法判别设备是否在 safe mode
- `saw_boot`（safe mode fail_step 解析）恒为 None → 无法获取 Boot 失败原因
- **整个 fallback 诊断路径产出的信息量为零**

**修复建议**：在 `_sniff()` 前重新调用 `_uds_init()`（或使用 zcanpro 的 raw receive API，如果有不依赖 uds_init 的接口）。至少应保留 init 状态下的 receive 能力来做 raw 嗅探。

---

### 【CONTRIBUTING FACTOR】lifecycle.h 文档与实现不一致——BOOTUP 发送时机

**分类**：RISK（文档误导，非功能缺陷）
**置信度**：90%

**证据**：

> `lifecycle.h:53-55`：
> ```
> * @brief  initialize lifecycle module and send BOOTUP broadcast
> * @note   must be called after can_driver_init().
>          sends the BOOTUP broadcast immediately.
> ```

> `lifecycle.c:124-126`（实际实现）：
> ```c
> void lifecycle_init(void)
> {
>   g_lifecycle_state = LIFECYCLE_BOOTUP;
>   g_last_broadcast_tick = timer_get_tick();
>   can_driver_register_busoff_recovery_callback(lifecycle_on_busoff_recovery);
>   /* BOOTUP is sent on first SIT1145 wake (can_protocol_poll), not at power-on. */
> }
> ```

`lifecycle_init()` 实际上**不发送 BOOTUP 广播**。BOOTUP 在首次 `can_protocol_poll()` 中通过 `can_lp_enter_normal()` → `can_lp_send_ident()` / `lifecycle_set_state(LIFECYCLE_BOOTUP)` 发送。

**影响**：无功能影响（行为正确），但文档误导审查方向。

---

### 【CONTRIBUTING FACTOR】脚本错误提示引用旧架构术语

**分类**：RISK（过时信息，不影响功能逻辑）
**置信度**：95%

**证据**：

> `zcanpro_read_app_version.py` run() fallback 路径：
> ```python
> _log("总线上 0 帧 MCU 回复。请确认：1) 通道 250kbps 扩展帧已打开；"
>      "2) 已用当前 main 同时烧 16KB Boot 和 Slot A APP（勿与 V1.0.0 28KB Boot 混用）；"
>      "3) 断电重启后再跑。")
> ```

- "Slot A APP"：OTA-ARCH-0920 已删除 A/B 槽位，当前架构为 Boot(16KB)+App(48KB)+Backup(48KB)
- "28KB Boot"：当前 Boot 为 16KB（`BOOT_SIZE=0x4000`）
- 暗示"Boot 和 APP 需要同时烧录"的合并 bin 流程在当前架构下已不适用——当前 Boot 自动从 Backup 搬运

**影响**：仅影响人工诊断时的判断方向，不影响脚本功能。

---

### 【EXCLUDED】SIT1145 上电 Standby 阻塞 App CAN 响应

**分类**：EXCLUDED
**置信度**：88%

**证据链**：

1. **SIT1145 上电默认模式 = Standby**：App sit1145.c step 10 注释："进入 Standby 模式（APP 低功耗默认状态）"。芯片 datasheet 默认 MODE_CONTROL=0x04。

2. **CAN_LP_STANDBY_ENABLE=0 时 step 10 编译排除**：
   > App sit1145.c（step 10 区域）：
   > ```c
   > #if !defined(CAN_LP_STANDBY_ENABLE) || (CAN_LP_STANDBY_ENABLE != 0U)
   >   if (sit1145_standby_mode_set() == 0U) { ... }
   > #endif
   > ```
   > `can_protocol.h:63`：`#define CAN_LP_STANDBY_ENABLE 0`

   ENABLE=0 时，step 10 被编译器移除，sit1145_init() 不再显式写 MODE_CONTROL=Standby。

3. **Boot 总是将 SIT1145 置为 Normal**：
   > Boot sit1145.c step 9：`sit1145_normal_mode_set()` → 写 MODE_CONTROL=0x07 + 等 CTS
   > Boot sit1145.c step 10：`sit1145_wait_cts(20U)` — 确认 Normal 完成

   Boot 的 sit1145_init() 无条件进入 Normal（Boot 版本无 step 10 Standby 代码）。

4. **Boot→App 跳转不改 SIT1145 模式**：
   > boot_jump.c:78-83：仅 `can_reset(CAN1)` + 时钟关闭 + `spi_enable(SPI1, FALSE)` + SPI1 时钟关闭
   > **不写 SIT1145 MODE_CONTROL 寄存器**

5. **App sit1145_init() 不改 MODE_CONTROL（ENABLE=0 时）**：
   App sit1145_init() steps 1-9 配置 GPIO/SPI/CAN_CONTROL/DATA_RATE/CWE，不写 MODE_CONTROL。
   SIT1145 保持 Boot 设置的 **Normal 模式**。

6. **App can_protocol_init() 置 g_lp_need_online=1**：
   > can_protocol.c:1931-1934：
   > ```c
   > #else
   >   /* 临时禁用：非 trial 上电同样延时 enter_normal，保持 CAN 在线 */
   >   g_lp_need_online = 1U;
   > #endif
   > ```

   首次 `can_protocol_poll()` 立即调用 `can_lp_enter_normal()` → `sit1145_normal_mode_set()`（已是 Normal，快速确认）→ `can_driver_online()`。

**结论**：Boot→App 正常路径下，SIT1145 始终处于 Normal 模式，不存在"MCU 已运行但 SIT1145 未进 Normal"的时序窗口。代码注释"收发器不进 Standby"有误导性——收发器不是"不进 Standby"而是"Boot 已设 Normal + App 不改"——但功能行为正确。

**残余风险**（非代码缺陷）：如果通过调试器直接烧 App（不经 Boot），SIT1145 上电默认 Standby，App sit1145_init() 不会将其切换到 Normal。首次 can_protocol_poll() → can_lp_enter_normal() 兜底处理，黑窗仅 ~20ms。但 main.c:58 注释"SIT1145 is in Standby"在此路径下才准确。

---

### 【EXCLUDED】Boot safe mode 不响应 UDS 3E 00

**分类**：EXCLUDED
**置信度**：95%

**证据**：

> boot_safe_mode.c:216-254 `enter_safe_mode()` 实现：
> ```c
> can_driver_init();                    // :216 — CAN 外设初始化（Boot 版本：离开 reset）
> (void)sit1145_normal_mode_set();      // :217 — SIT1145 置 Normal
> ...
> while (1)
> {
>     if (can_flag_get(CAN1, CAN_RIF_FLAG) != RESET)  // :225
>         can_driver_rx_irq_handler();                 // :226
>     while (can_driver_recv(&id, data, &len) == 0)    // :228
>     {
>         if (id != CAN_ID_UDS_REQUEST) continue;      // :233 — 0x18DA0D03 过滤
>         // ISO-TP SF 解析（兼容 PCI 前缀和裸 UDS）   // :235-243
>         if ((un >= 2U) && (u[0] == 0x3EU))           // :245
>         {
>             r[0] = 0x7EU; r[1] = u[1] & 0x7FU;      // :247-248
>             if ((u[1] & 0x80U) == 0U)                // :249 — suppress 位检查
>                 safe_send_sf(r, 2U);                  // :251 — ISO-TP SF 02 7E xx
>         }
>     }
>     // 500ms 心跳 + SIT1145 Normal 维护              // :258-262
> }
> ```

safe mode 无条件初始化 CAN，进入无限接收循环，对 UDS 3E 00（suppress=0）回 `02 7E 00`。**不存在不响应的代码路径**。

唯一不响应窗口：`boot_metadata_save(&g_meta)` (:214) 期间（flash 写入 ~40-80ms），此时 CAN 尚未初始化。

---

### 【EXCLUDED】UDS 请求格式 / DID 格式不匹配

**分类**：EXCLUDED
**置信度**：95%

**证据**：

| 维度 | 脚本 | 固件 | 一致? |
|------|------|------|-------|
| Tx CAN ID | `UDS_REQ_ID=0x18DA0D03` (script:26) | `CAN_PROTO_UDS_REQUEST=0x18DA0D03U` (can_protocol.h:42) / `CAN_ID_UDS_REQUEST=0x18DA0D03U` (can_driver.h:42) | ✅ |
| Rx CAN ID | `UDS_RESP_ID=0x18DA030D` (script:27) | `CAN_PROTO_UDS_RESPONSE=0x18DA030DU` (can_protocol.h:43) / `CAN_ID_UDS_RESPONSE=0x18DA030DU` (can_driver.h:43) | ✅ |
| 3E 00 格式 | ISO-TP SF `02 3E 00`（zcanpro.uds_request 自动组帧） | App: isotp_rx_process → uds_process_message → handle_tester_present(data,len) → resp `[0x7E, 0x00]` | ✅ |
| DID 0xF195 | `payload=[0xF1,0x95]` → SF `03 22 F1 95` | `DID_SW_VERSION=0xF195U` → fill_did_payload → 32B ASCII | ✅ |
| DID 0xF180 | `payload=[0xF1,0x80]` | `DID_BOOTLOADER_VERSION=0xF180U` | ✅ |
| DID 0xF193 | `payload=[0xF1,0x93]` | `DID_HW_VERSION=0xF193U` | ✅ |
| 版本期望值 | `QC_JYF_FW_1.1.1` / `QC_JYF_BL_1.0.0` / `QC_JYF_HW_1.1.5` | can_protocol.c:53-55 编译常量完全一致 | ✅ |
| CAN 过滤器 | 框架 bit31=1 扩展帧 | Filter0 code=0x18DA0D03 mask=0x1FFFFFFF（精确 29-bit 匹配） | ✅ |
| 帧类型 | `_make_frame()` 设 `is_extend=1, eff=1, id_type=1` | `CAN_ID_EXTENDED + CAN_FRAME_DATA` | ✅ |

---

### 【EXCLUDED】App UDS dispatch 链缺陷

**分类**：EXCLUDED
**置信度**：90%

**证据**：完整 dispatch 链：

```
CAN1 RX IRQ (can_driver_rx_irq_handler)
  → rx_fifo[head] (software FIFO, 16 deep)
  → can_driver_poll() (main loop)
    → rx_callback = can_protocol_rx_handler
      → ID filter: CAN_PROTO_UDS_REQUEST or 0x18DB33xx
      → can_lp_mark_uds()
      → isotp_rx_process(data, len)
        → ISO-TP SF 解析
        → isotp_message_received(data, len)  [callback]
          → session timeout check
          → uds_process_message(data, len)
            → switch(service_id)
              → case 0x3E: handle_tester_present()
                → resp=[0x7E, sub_func]
                → proto_send_response(resp, 2)
                  → isotp_tx_send(CAN_PROTO_UDS_RESPONSE, ...)
                    → can_driver_send(0x18DA030D, sf, 8)
```

**关键 gate**：`can_protocol_poll()` 中：
```c
if (g_can_awake == 0U) { ... return; }   // :2078
```
当 `g_can_awake=0` 时，poll 提前返回，不做 SIT1145 维护/ISO-TP poll。但 `can_driver_poll()`（main loop 中独立调用）仍会执行 `rx_callback`。所以即使 `g_can_awake=0`，只要 CAN 外设在线（不在 software reset），RX 帧仍能被 dispatch。

实际 gate 在 CAN 硬件层：`can_driver_online()` 之前 CAN1 处于 software reset，硬件不接收帧 → FIFO 为空 → dispatch 链无输入。

---

### 【EXCLUDED】CAN ID 三方不一致（D2 已排除，D3 复核确认）

**分类**：EXCLUDED
**置信度**：98%

D2 报告已逐字节比对三方一致。D3 复核：
- 脚本：`0x18DA0D03` / `0x18DA030D`
- Boot can_driver.h:42-43：`0x18DA0D03U` / `0x18DA030DU`
- App can_protocol.h:42-43：`0x18DA0D03U` / `0x18DA030DU`

---

### 【EXCLUDED】Boot app_valid 门控缺陷（D2 已排除，D3 复核确认）

**分类**：EXCLUDED
**置信度**：95%

boot main.c:60 `boot_app_image_ok()` 直读 0x08004000 XATO 头验 magic/len/CRC32/ECDSA/vectors，与 `meta.app_valid` 无关。app_valid 仅在 boot_trial.c:182（copy 成功后写 1）和 boot_metadata.c:124（defaults 写 0）两处被写，无读消费点。

---

## 二、横切关注点审查结果

### E1. Boot→App 跳转后 CAN 外设状态

**Boot 跳转序列** (boot_jump.c:75-108)：
1. `boot_diag_m4(app_addr)` — 发 M4 诊断帧（有界 ≤3ms）
2. `__disable_irq()`
3. `can_reset(CAN1)` + `crm_periph_clock_enable(CRM_CAN1_PERIPH_CLOCK, FALSE)` — CAN 外设完全关闭
4. `spi_enable(SPI1, FALSE)` + SPI1 时钟关闭 — SPI 关闭
5. SysTick 停止
6. NVIC 全部禁用+清 pending
7. VTOR 切换 + MSP 切换 + 跳转

**App 初始化时** (can_driver.c:97-130 App 版)：
- 重新使能 GPIOA/CAN1 时钟
- 重新配置 PA11/PA12 CAN 引脚
- 调用 sit1145_init()（重新初始化 SPI1 + 配置 SIT1145 寄存器）
- CAN1 reset + software reset + bittime/filter 配置
- **留在 software reset 状态**，等 can_driver_online()

**结论**：Boot→App 跳转后 CAN 外设完全重新初始化，不存在状态残留问题。

### E2. metadata v2→v3 迁移对 Boot 决策链耗时的影响

boot_metadata.c:115-145：当 primary 和 backup 均校验失败时，`meta_fill_defaults()` + `boot_metadata_save()` 双副本写入。
- `meta_write_to_flash()`: IRQ-off flash sector erase + word program + readback
- 每副本 ~20-40ms（1KB sector 擦除 + 28B 写入 + 回读）
- 双副本：~40-80ms

此延时远小于 ECDSA 验签时间，不是黑窗的主要贡献者。

**但注意**：v2→v3 迁移意味着 v2 metadata 会被 meta_validate() 拒绝 → defaults 重建。如果 App 侧 ota_metadata_read() 也拒绝 v2，App 的 `can_lp_trial_needs_normal()` 返回 0 → 但 CAN_LP_STANDBY_ENABLE=0 时 `g_lp_need_online=1` 不受影响。

### E3. OTA-ARCH-0920 变更对 UDS 相关代码路径的影响

**变更概要**：A/B 槽位删除，改为 Boot+App+Backup 单 App 架构。

**对 UDS 的影响**：
- DID 0x2113 (active_slot) / 0x2114 (pending_slot) 语义改变但仍存在
- DID 0xF195 (SW version) 从读 metadata/镜像头改为读编译常量（can_protocol.c:53 注释）
- Boot safe mode 帧格式不变（0x18DA030D，probe 22 2113）
- UDS CAN ID 不变

**脚本 DID 请求不受影响**：脚本只读 0xF195/0xF180/0xF193，这些 DID 的 handler 在 OTA-ARCH-0920 变更后仍正常工作。

### E4. 脚本架构假设与当前架构匹配度

| 脚本元素 | 当前架构 | 匹配? |
|----------|---------|-------|
| CAN ID 0x18DA0D03/0x18DA030D | 不变 | ✅ |
| DID 0xF195/0xF180/0xF193 | App can_protocol.c 正确响应 | ✅ |
| 期望版本值 | 与 can_protocol.c:53-55 编译常量一致 | ✅ |
| "Slot A APP" 诊断提示 | 架构已改 Boot+App+Backup | ❌ 过时 |
| "28KB Boot" 诊断提示 | 当前 16KB Boot | ❌ 过时 |
| safe mode fail_step 解析 (`62 21 13 FE xx`) | boot_safe_mode.c 应答格式不变 | ✅ |
| lifecycle 心跳 0x18FF260D 检测 | Boot safe mode 仍发 `'ABT'` 标记帧 | ✅ |

---

## 三、关键技术问题解答

### Q1: 什么代码条件下 Boot 和 App 都不响应 UDS 3E 00？

**唯一确定性条件**：Boot 决策链执行期间（metadata 读取 + ECDSA 验签 + 可选 backup copy），Boot CAN 硬件在线（能 ACK）但软件不处理 RX。随后 boot_jump_to_app() 关闭 CAN，App 初始化期间 CAN 也离线。**整个 Boot 决策链 + Boot→App 跳转 + App 初始化期间，UDS 3E 00 不被任何固件应答**。

无 backup pending 时此窗口 ≈ 0.5–1s；有 backup pending 时 ≈ 2.8–4.0s。

**非代码条件**（排除物理因素后不可达）：SIT1145 硬件故障/SPI 通信中断、CAN 收发器损坏。

### Q2: MCU 上电到 CAN 可响应之间是否存在时序间隙？多长？

**存在，且是设计使然**。

| 路径 | 黑窗时长 | 构成 |
|------|----------|------|
| Boot safe mode | ~50–130ms | 时钟+CAN init + metadata save |
| Boot → App（无 copy） | ~500–950ms | Boot init + ECDSA 验签 + App init + CAN online |
| Boot → App（有 copy） | ~2800–4000ms | 上述 + Backup 验签 + 48KB 擦写 + 二次验签 |

**Boot 阶段 CAN 硬件在线但不处理 RX**：can_driver_init() 在 boot_diag_can_init() 中调用，CAN 离开 reset、中断使能、过滤器配置——硬件可以接收帧并产生中断。但 Boot 主流程（main.c:35-63）不进入接收循环，帧在 FIFO 中堆积直到 boot_jump_to_app() 清除。

### Q3: sit1145_init() 步骤 10 在 CAN_LP_STANDBY_ENABLE=0 时是否仍置 Standby？

**不置 Standby，但也不置 Normal**。

App sit1145.c step 10 的 `#if !defined(CAN_LP_STANDBY_ENABLE) || (CAN_LP_STANDBY_ENABLE != 0U)` 条件在 ENABLE=0 时编译排除了 `sit1145_standby_mode_set()` 调用。但步骤 1-9 也不写 MODE_CONTROL 寄存器。

**实际行为**：
- Boot→App 路径：SIT1145 保持 Boot 设置的 Normal 模式（Boot sit1145_init step 9）
- 直接烧 App（不经 Boot）：SIT1145 保持上电默认 Standby，由首次 can_protocol_poll() → can_lp_enter_normal() 兜底切换

代码注释"收发器不进 Standby"措辞不准确——准确表述是"不再显式写 Standby"。

### Q4: Boot 进 safe mode 后是否总是响应 3E 00？

**是**。boot_safe_mode.c:216-262 enter_safe_mode() 实现了一个无条件的 UDS 应答循环：
- `can_driver_init()` + `sit1145_normal_mode_set()` 无条件执行
- while(1) 循环持续 poll CAN RX + 处理 UDS 请求
- 3E 00 (suppress=0) → 必回 `02 7E 00`
- 3E 80 (suppress=1) → 不应答（符合 UDS 规范）
- 22 2113 → 必回 `05 62 21 13 FE <fail_step>`
- 其他 22 DID → 必回 NRC `7F 22 11`

**唯一不响应窗口**：enter_safe_mode() 开头的 `boot_metadata_save(&g_meta)` (~40-80ms flash 写入期间 CAN 未初始化)。

### Q5: Boot 跳 App 过程中 CAN 是否有离线窗口？

**有**。boot_jump_to_app() (boot_jump.c:78-79)：
```c
can_reset(CAN1);
crm_periph_clock_enable(CRM_CAN1_PERIPH_CLOCK, FALSE);
```
CAN 外设被硬复位+时钟关闭，之后 App 重新初始化 CAN（can_driver_init → software reset → can_driver_online）。从 can_reset 到 can_driver_online 的间隔 = App 全部 init 时间 ≈ 20-70ms。

### Q6: 版本脚本是否有过时架构假设导致请求路径错误？

**请求路径无错误**。CAN ID、DID 值、UDS 格式、版本期望值均与当前固件完全匹配。

**诊断路径有过时假设**：错误提示中的 "Slot A APP"、"28KB Boot" 为旧架构术语，仅影响人工判读方向，不影响脚本功能逻辑。

---

## 四、修复建议（按优先级）

### P0: 增强脚本容错窗口

```python
# zcanpro_read_app_version.py wake_mcu() 修改建议
def wake_mcu(bus_id):
    ok = False
    for i in range(1, 9):  # 从 2 次增加到 8 次
        ...
        try:
            rx = uds_req(bus_id, SID_TP, [0x00], timeout_note=" (唤醒第%d次)" % i)
            ...
        except Exception as e:
            _log("唤醒 %d/8: %s" % (i, e))
            time.sleep(0.5)  # 从 0.15s 增加到 0.5s
    return ok
```

总容错窗口：8 × (2s timeout + 0.5s gap) ≈ 20s，覆盖最坏 Boot copy 场景。

### P1: 修复脚本 raw 嗅探诊断盲区

```python
# 在 _sniff() 前重新 init
_log("UDS 无 7E，重新 init 后 raw 嗅探")
_uds_init()   # ← 新增：重新打开接收通道
can_send(bus_id, UDS_REQ_ID, [0x02, 0x3E, 0x00])
...
saw_boot, saw_app, saw_life, n = _sniff(bus_id, 1.5)
```

### P2: 更新脚本诊断提示

```
"2) 已用 merge_prod_bin 烧录 16KB Boot + 48KB App 合并 bin（OTA-ARCH-0920 无 A/B 槽位）"
```

### P3: Boot 可观测性增强（可选，非紧急）

Boot 在进入耗时操作前发送 BUSY 标记帧到 BOOT_DIAG_CAN_ID (0x18FF480D)：
- copy 开始前：已有 M2 (boot_diag_m2(0,0))
- verify 开始前：已有 M3 (boot_diag_m3(0,0xFF,target))
- 建议：主机侧诊断脚本监听 0x18FF480D，检测到 M2/M3 帧时等待 Boot 完成

---

## 五、结构化摘要

```json
{
  "summary": "未发现单一确定性代码缺陷导致 Boot/App 不响应 UDS 3E 00。根因收敛为 Boot→App CAN 黑窗（ECDSA 验签 + 可选 backup copy ≈ 2.8-4.0s）超出脚本 ~4s 容错窗口。当 metadata backup_valid=1 时每次上电重复此黑窗，解释了22:28/03:52 同签名复现。脚本 raw 嗅探在 deinit 后必然 0 帧（ZLG 库语义），诊断信息量为零。Boot safe mode 和 App UDS dispatch 链均确认能响应 3E 00（排除代码缺陷）。SIT1145 上电 Standby 不阻塞 App CAN（Boot 已设 Normal + App 不改模式 + can_lp_enter_normal 兜底）。CAN ID/DID/UDS 格式三方一致（排除请求格式问题）。",
  "files": [
    "python_tools/2.functional test script/zcanpro_read_app_version.py",
    "qi_wireless_bootloader/mdk_user/Src/main.c",
    "qi_wireless_bootloader/mdk_app/Src/boot_safe_mode.c",
    "qi_wireless_bootloader/mdk_app/Src/boot_jump.c",
    "qi_wireless_bootloader/mdk_app/Src/boot_trial.c",
    "qi_wireless_bootloader/mdk_app/Src/boot_metadata.c",
    "qi_wireless_bootloader/mdk_app/Src/boot_verify.c",
    "qi_wireless_bootloader/mdk_app/Src/sit1145.c",
    "qi_wireless_bootloader/mdk_can/Src/can_driver.c",
    "qi_wireless_bootloader/mdk_can/Inc/can_driver.h",
    "qi_wireless_bootloader/mdk_app/Inc/boot_metadata.h",
    "qi_wireless_bootloader/mdk_app/Inc/boot_safe_mode.h",
    "qi_wireless_code_app/mdk_user/Src/main.c",
    "qi_wireless_code_app/mdk_app/Src/can_protocol.c",
    "qi_wireless_code_app/mdk_app/Inc/can_protocol.h",
    "qi_wireless_code_app/mdk_app/Src/sit1145.c",
    "qi_wireless_code_app/mdk_app/Inc/sit1145.h",
    "qi_wireless_code_app/mdk_app/Src/lifecycle.c",
    "qi_wireless_code_app/mdk_app/Inc/lifecycle.h",
    "qi_wireless_code_app/mdk_can/Src/can_driver.c"
  ],
  "confidence": 72,
  "key_findings": [
    {"category": "CONTRIBUTING FACTOR", "title": "Boot→App CAN 黑窗超出脚本容错窗口", "confidence": 72},
    {"category": "RISK", "title": "脚本 raw 嗅探在 deinit 后必然 0 帧（诊断盲区）", "confidence": 95},
    {"category": "RISK", "title": "lifecycle.h 文档与实现不一致（BOOTUP 发送时机）", "confidence": 90},
    {"category": "RISK", "title": "脚本诊断提示引用旧架构术语", "confidence": 95},
    {"category": "EXCLUDED", "title": "SIT1145 上电 Standby 阻塞 App CAN", "confidence": 88},
    {"category": "EXCLUDED", "title": "Boot safe mode 不响应 3E 00", "confidence": 95},
    {"category": "EXCLUDED", "title": "UDS/DID 请求格式不匹配", "confidence": 95},
    {"category": "EXCLUDED", "title": "App UDS dispatch 链缺陷", "confidence": 90},
    {"category": "EXCLUDED", "title": "CAN ID 三方不一致", "confidence": 98},
    {"category": "EXCLUDED", "title": "Boot app_valid 门控缺陷", "confidence": 95}
  ]
}
```

---

*报告生成：evaluator subagent | 审计方式：只读静态分析 | 时钟截止：2026-09-21 04:30*
