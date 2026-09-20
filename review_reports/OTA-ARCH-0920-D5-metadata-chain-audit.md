# OTA-ARCH-0920-D5 — metadata 标记→Boot 搬运链路审计

- 审计时间：2026-09-21 05:15–05:40（只读，零改码）
- 触发症状：04:40 OTA 升级，0x37→77 成功、41096/41096 写入 Backup，复位后
  0x2113=0x00 / 0xF195=QC_JYF_FW_1.1.1（旧版）/ 0x2114=0x02 / 0x2115=0x00 / 0x2116=0
- 审计范围：APP metadata 写入链（ota_download.c/ota_trigger.c）→
  双工程结构一致性（ota_trigger.h vs boot_metadata.h）→
  Boot 判定/搬运链（boot_metadata.c/boot_trial.c/main.c）→
  DID 数据源（can_protocol.c）→ 复位时序
- 对照基线：用户预期流程（commit→复位→Boot 擦 App+搬运→清标记→跳转）

---

## 结论摘要

**verdict: ROOT CAUSE CONFIRMED（部署层断点）+ RETEST BLOCKER（P0 潜伏缺陷）**

被审计的代码链路（APP 写入 → 结构一致性 → Boot 判定/搬运逻辑）**逐环节闭合、无逻辑断点**：
commit_backup 落盘被 0x77 正应答门禁证明已完成；双工程 v3 结构逐字节一致；
Boot 状态机在 backup_valid=1 前提下**不存在任何不落盘 0x03/0x04 的路径**。

实测 DID 状态（0x2114=0x02 ∧ 0x2115=0x00 ∧ 0x2116=0）经状态机穷举证明
**在当前源码编译出的 Boot 上不可达** ⇒ 链条断点不在代码逻辑，而在**部署态**：

> **设备上运行的 Boot 二进制 ≠ 当前 qi_wireless_bootloader 源码（陈旧 Boot ROM）。
> Boot 决策链第 3 步（backup_valid 判定 → copy）从未执行。**

制品时间线强力佐证：仓库内 `Objects/bootloader.bin` 构建于 **2026-09-20 09:23**，
早于 OTA-ARCH-0920 架构重构首个 commit（6e9ab04，17:04）约 8 小时；
`merge_prod_bin.py` 的 `--boot` 默认值正是这个陈旧文件；CAN OTA 链路**设计上无法更新 Boot**。

同时发现 **P0 潜伏缺陷**：`boot_trial.c copy_erase_app_region()` 擦除 App 区前
**漏调 flash_unlock()**（全工程唯一），烧录新 Boot 后重测必然在搬运第 2/3 步失败，
必须先修再测。

confidence: **88**（逻辑排除链闭合于源码；设备物理 flash 内容无法从本机直接检视；
陈旧 Boot 的确切年代（A/B 重构前 vs 更早工厂 ROM）存在两种候选，但不影响结论与修复方案）

---

## 状态机排除证明（核心推理）

当前源码下，flash metadata 的 DID 读数与 Boot 行为的映射是确定的：

| 前提（复位时 flash 状态） | Boot 执行路径 | 复位后 flash 落点 | 对应 DID |
|---|---|---|---|
| backup_valid=1 | boot_copy_backup 全部 6 条出口（boot_trial.c:130/142/152/164/176/187）**每条都调用 boot_metadata_save** | last_boot_reason ∈ {0x03,0x04}，retry_count≥1（成功时 +backup_valid=0） | 0x2115∈{03,04} |
| backup_valid=0 | main.c:56 无 pending 分支，不写 flash | 维持 APP 写入值 | 0x2114=0xFE |
| 双副本校验失败 | boot_metadata.c:188-190 defaults 重建 + save | backup_valid=0, reason=0x00 被写入 | 0x2114=0xFE（APP 侧同样校验失败）|
| Boot 搬运中挂死 | 无跳转，设备死亡 | — | DID 无应答 |

实测 = **0x2114=0x02（backup_valid=1 且 crc 校验通过）∧ 0x2115=0x00 ∧ 0x2116=0 ∧ 设备存活**：

- 排除行 1：任何 copy 路径必写 0x03/0x04 + retry≥1，与实测矛盾；
- 排除行 2：与 0x2114=0x02 矛盾；
- 排除行 3：APP 侧 ota_metadata_read 与 Boot 侧 meta_validate 算法逐字节相同
  （ota_trigger.c:22-40 ≡ boot_metadata.c 84-115），Boot 校验不过 ⇒ APP 也校验不过 ⇒ 0x2114=0xFE，矛盾；
- 排除行 4：设备存活且旧 App 正常应答 DID，矛盾。

**唯一剩余解释：Boot 执行的二进制不含上述状态机——即设备 Boot 早于 6e9ab04 架构重构。**
佐证：metadata 双副本保持 APP 写入原值（0x2114=0x02 持续可读）⇒ 本次开机
**没有任何 Boot 代码写过 0x0801C000/0x0801C800**。

---

## 发现项明细

### [ROOT CAUSE] D5-1 设备 Boot 二进制陈旧，copy 引擎从未执行

- 断点：Boot 决策链 main.c:39-56（backup_valid 判定 → boot_copy_backup）从未运行
- 证据链：
  - 状态机排除证明（上节）；boot_trial.c:115-191 六条出口全落盘（130/142/152/164/176/187）
  - `qi_wireless_bootloader/mdk_project/Objects/bootloader.bin` mtime = **2026-09-20 09:23:02**
  - git 时间线：c502f19 (A/B 时代修复) 09-20 11:43 → 6e9ab04 (架构重构，copy 引擎诞生,
    META_VERSION=2) 17:04 → d3b550d (v3 瘦身) 18:31 → 7581fed 18:46 —
    **仓库内唯一 Boot 制品早于全部重构 commit**
  - `python_tools/1.packaging script/merge_prod_bin.py:29-31` DEFAULT_BOOT 默认指向
    该陈旧 Objects/bootloader.bin；docs/11. 签名校验与脚本使用.md:220 同
  - `python_tools/burn bin/`、`app bin/` 目录为空 —— 无任何重构后的合并制品可考
  - ota_download.c:13 设计注释明确："BOOT performs the physical copy"——
    CAN OTA 只更新 Backup/App 区，**Boot 区 0x08000000..0x08003FFF 只能 SWD/ISP 整片烧录**
- 判据价值：本条同时解释全部实测症状——旧 App 版本（无搬运）、0x2114=0x02（metadata 无人消费）、
  0x2115=0x00（陈旧值残留）、0x2116=0（retry 计数从未增）
- 附注：若设备 Boot 为 v2 时代（6e9ab04~d3b550d 之间）构建，其 meta_validate 期望
  META_VERSION=2（git show d3b550d^:boot_metadata.h:54）且校验失败会 defaults 重建+save
  （同文件 189-191 行），将破坏 v3 副本 ⇒ 0x2114 应读 0xFE，与实测矛盾；
  故设备 Boot 实际年代 ≤ 6e9ab04 之前（或其 metadata 基址/写入行为不触及 0x0801C000）。
  确切年代属二分细节，不改变结论。

### [CONTRIBUTING] D5-2 部署链缺口：Boot 更新无 OTA 路径 + 打包默认吃陈旧制品

- merge_prod_bin.py:29-31/71-72 --boot 默认 = Objects/bootloader.bin（陈旧）
- docs/11:232 合并产物从 0x08000000 整片烧录（产线 J-Link/ISP），CAN 升级永远碰不到 Boot 区
- 后果：APP 侧迭代越快（09-20 一天内 7 个 commit），设备 Boot 越可能停留在产线/上次全片烧录的年代；
  "脚本升级成功但 Boot 不搬运"是该缺口的必然形态，而非偶然故障

### [CONTRIBUTING] D5-3 Boot 身份不可证实（UDS 盲区）

- DID 0xF180（bootloader 版本）由 **APP 编译时常量**应答：
  qi_wireless_code_app/mdk_app/Src/can_protocol.c:54 `BOOTLOADER_VER_STR = "QC_JYF_BL_1.0.0"`，
  fill_did_payload case DID_BOOTLOADER_VERSION（can_protocol.c:677-679）直接回字符串，
  **不读 Boot ROM**；Boot 侧无任何版本 DID（boot_safe_mode.c 仅在 safe-mode 应答 22 21 13）
- zcanpro_ext_ota_auto.py 全文无 0xF180 探测（grep 零命中）——脚本层也没有闸门
- 后果：本次根因（陈旧 Boot）**在 UDS 层完全不可见**，判别矩阵无行可归（见 D5-6）

### [RISK-HIGH / RETEST BLOCKER] D5-4 copy_erase_app_region 漏 flash_unlock + 漏 IRQ 屏蔽

- 位置：qi_wireless_bootloader/mdk_app/Src/boot_trial.c:60-75（擦 App 区 0x08004000..0x0800FFFF）
- 对比：全工程其余 flash 写/擦点全部先解锁——
  - ota_trigger.c:120-122 `__disable_irq(); flash_unlock();`（metadata 写）
  - boot_metadata.c meta_write_to_flash（boot 侧 metadata 写，同款）
  - boot_trial.c:90-91 copy_program_region `__disable_irq(); flash_unlock();`（搬运第 3 步）
  - ota_download.c ota_dl_handle_erase（APP 0x31 擦 Backup 区）`__disable_irq(); flash_unlock();`
- 库行为证据：at32f422_426_flash.c:172-187 `flash_sector_erase()` **不含自解锁**——
  直接置 FLASH->ctrl_bit.secers/erstr 并 flash_operation_wait_for 轮询
- 挂锁推演：复位后 flash 控制器默认上锁；metadata primary 有效时 boot_metadata_init
  不写 flash（boot_metadata.c:171-175 主区命中直接 return）⇒ 搬运第 2 步执行时 flash 处于锁定态
  ⇒ 擦除操作被静默忽略（无 busy 无错误 ⇒ wait_for 可能返回 DONE）⇒ 第 3 步 copy_program_region
  解锁后对**未擦除**的旧 App 区编程 ⇒ 旧代码位为 0 处无法回 1 ⇒ 逐字回读校验失败 ⇒
  copy_fail_step=0xFD（boot_trial.c:161）⇒ COPY_FAIL 落盘、flag 保留、每 boot 重试
- 双缺陷叠加：擦除同样缺 __disable_irq——违反 incident 4ec5858 单 bank 取指纪律
  （ota_trigger.c:144-146 / boot_metadata.c 注释均以该事故为据），存在核心挂死风险
- **重测影响：不修此条，烧录新 Boot 后 OTA 仍将以 0x2115=0x04 / fail_step=0xFD|0xFE 失败**

### [RISK] D5-5 正常开机不落盘 boot reason —— DID 0x2115 语义先天失真

- main.c:36 `g_meta.last_boot_reason = detect_boot_reason();` 仅 RAM 赋值；
  无 pending 分支（main.c:54-57）全程不调 boot_metadata_save
- 即：0x2115 只反映**上一次 copy 路径/defaults 重建**的落盘值或陈旧残留，
  "0x2115=0x00=POR" 在正常开机后**恒真且不可证伪**
- 用户预期"Boot 未走 copy 应见 0x03/0x04"只有在 Boot 确实进入 copy 路径时才成立——
  本次恰是用 0x2114=0x02（backup_valid 持续存在）反证 Boot 没进过任何落盘路径

### [RISK] D5-6 上位机判别矩阵无 stale-Boot 行 + M2 诊断帧解码错位

- zcanpro_ext_ota_auto.py:1194-1204 判别矩阵四行均以"设备 Boot 为新架构"为隐含前提；
  实测组合（2113=0x00 ∧ 2114=0x02 ∧ 2115=0x00 ∧ 2116=0 ∧ 版本旧）落入
  "2114=目标槽 → trial PENDING 已落盘但设备未复位 → 断电重启后重跑"——
  **该指引永不收敛**（重启多少次 Boot 都不会搬运）
- boot_safe_mode.h:35-36 文档化 M2 帧格式 `[copy result][detail]`，
  但实现 boot_safe_mode.c boot_diag_m2 发送 `p[1]=info(detail), p[2]=ret(result)`——
  **字节序与文档相反**；zcanpro_boot_diag_capture.py 解码表照抄头文件
  （"M2 0xA2: [copy result][detail]"）⇒ 重测抓帧时 committed（wire [A2][00][01]）
  会被误读为 result=0（"start"），fail_step 帧会被误读——恰好错判关键判别帧
- 修复前置：头文件注释 / 实现 / 抓帧脚本解码表三处对齐后再重测抓帧

### [RISK] D5-7 ECDSA 公钥双源无同步机制（潜伏）

- APP 侧验签回退键 g_app_ecdsa_pubkey（can_protocol.c:58-71）与
  Boot 侧 g_ecdsa_public_key（boot_verify.c:30-42）**当前逐字节一致**（04 79 0D 96 ... 71），
  且双方都优先取 DeviceInfo 键（ota_download.c image_pubkey() /
  boot_verify.c boot_verify_get_public_key）——今日无分歧
- 但两键分属两工程手工维护（can_protocol.c:60 注释"same public key as Bootloader
  boot_verify.c"仅为口头约束），无构建期一致性校验；
  一旦 DeviceInfo 未烧键且两侧 .rodata 漂移 ⇒ APP 0x37 验签通过而 Boot fail_step=6 拒搬运
  （该形态表现为 0x2115=0x04/retry≥1，与本次症状不同，属重测后的下一风险点）

### [EXCLUDED] D5-A APP metadata 写入链（任务 A 全部关键问题）

- commit_backup（ota_download.c:261-277）：ota_metadata_read 失败时自建 magic/version；
  写 backup_valid=1（:273）、backup_crc32=hdr->crc32（:274）、ota_state=IDLE → ota_metadata_save
- ota_metadata_save（ota_trigger.c:162-176）：先重算 crc32（:167）→ 先写备份副本
  0x0801C800（:169）→ 再写主副本 0x0801C000（:173）；每副本 IRQ-off + 擦 + 字编程 +
  **逐字回读校验**（ota_trigger.c:144-155），失败即 -1
- "复位前是否落盘"：**已证明落盘**——ota_dl_poll（ota_download.c:589-617）顺序为
  verify（:589）→ commit_backup!=0 则 NRC（:595）→ g_trial_ready=1（:602）→
  0x77 应答（:605）→ TX idle → SHUTDOWN → NVIC_SystemReset（:617）。
  **0x77 在总线上出现 = 双副本已写入且逐字回读通过**，复位不可能先于落盘
- 结构布局与 Boot 相同：见 D5-B
- 结论：APP 写入链无断点，[EXCLUDED]

### [EXCLUDED] D5-B 双工程结构一致性（任务 B 全部关键问题）

- ota_trigger.h:88-101 vs boot_metadata.h:76-89：字段、顺序、偏移注释逐项一致——
  magic@0x00=0x4F54414D、version@0x04=3、app_valid@0x08、backup_valid@0x09、
  copy_fail_step@0x0A、last_boot_reason@0x0B、backup_crc32@0x0C、copy_retry_count@0x10、
  ota_state@0x14、reserved[3]@0x15、crc32@0x18，sizeof=28B
- META_VERSION：两侧均 3U（ota_trigger.h:47 / boot_metadata.h:57 附近）；magic 一致
- crc32 计算：两侧 META_CRC32_OFFSET 均 = sizeof-4 = 24B（0x00..0x17）；
  ota_crc32（ota_trigger.c:62-79）与 boot_crc32/boot_crc32_continue
  （boot_metadata.c:121-147）同一实现：poly 0xEDB88320、init 0xFFFFFFFF、xorout 0xFFFFFFFF
- d3b550d v3 瘦身同时改两侧（git show --stat：boot_metadata.h/c + ota_trigger.h/c 同 commit），
  后续 aaa7d91/7581fed 为 typedef 恢复与注释收尾，无布局增量
- 结论：v3 逐字节一致，[EXCLUDED]

### [EXCLUDED] D5-C Boot 校验/判定逻辑本身（任务 C 关键问题）

- boot_metadata_init（boot_metadata.c:164-194）：primary→backup（命中即回写双副本 :182）→
  defaults 重建并落盘（:188-190）；校验 = magic + version==3 + crc24（:105/:110）
- "APP 写入未过 Boot 校验会怎样"：defaults 重建 + save ⇒ backup_valid=0 落盘 ⇒
  与实测 0x2114=0x02 矛盾——该分支本次**未发生**（也侧面证明双副本自洽）
- boot_backup_pending（boot_trial.c:52-55）：backup_valid != 0 即 pending，无附加门禁
- copy 前验签：**有**——boot_copy_backup 第 1 步（boot_trial.c:124）对 Backup 区完整
  boot_verify_image（boot_verify.c:87-173：magic/length/CRC32/reset-vector 窗口/ECDSA P-256），
  且第 0 步 M3 pre-verify 标记；验签失败 → copy_fail_step=g_verify_fail_step、retry++、
  COPY_FAIL 落盘（boot_trial.c:127-130）、flag 保留、下次 boot 重试（幂等设计正确）
- 结论：Boot 源码逻辑闭合；该环节非断点，[EXCLUDED]

### [EXCLUDED] D5-D DID 数据源歧义（任务 D 关键问题）

- 0x2113（DID_ACTIVE_SLOT）：ota_running_slot() 兼容 shim，恒 0（ota_trigger.c:182-185），
  非 metadata 字段
- 0x2114（DID_PENDING_SLOT，can_protocol.c:715-728）：复位后 g_erased 为 .bss 静态量
  （ota_download.c:64），启动代码清零 ⇒ ota_dl_erased()=0 ⇒ 走 else 分支 =
  **flash metadata backup_valid 经 ota_metadata_read 校验后的映射**（:725：
  backup_valid≠0 → 0x02，否则 0xFE；读失败 → 0xFE）。
  ⇒ 实测 0x02 = flash 中 v3 metadata 校验通过 ∧ backup_valid=1，**非运行时残留**
- 0x2115（DID_LAST_BOOT_REASON，can_protocol.c:730-735）：= flash metadata.last_boot_reason
  （读失败报 0xFF，实测 0x00 ⇒ 读成功且值确为 0）
- 0x2116（DID_ROLLBACK_COUNT，can_protocol.c:737-742）：= flash copy_retry_count
- 0xF195（DID_SW_VERSION，can_protocol.c:667-676）：运行中 APP 的编译时常量
  SW_VERSION_STR（can_protocol.c:53 = "QC_JYF_FW_1.1.1"）——只反映 App 区正在运行的镜像
- 结论：数据源全部明确，用户对 0x2114 的解读正确，[EXCLUDED]

### [EXCLUDED] D5-E 断电/复位时序（任务 E 关键问题）

- APP commit_backup（双副本落盘+回读）完成于 0x77 应答之前（ota_download.c:595→605）；
  0x77 之后 wait_tx_idle(50) + SHUTDOWN 生命周期帧 + wait_tx_idle(20) 才 NVIC_SystemReset（:617）
- 脚本侧 uds_ecu_reset（zcanpro_ext_ota_auto.py:799-829）：0x37 后的 11 01 为兼容性补发，
  即使先于 APP 自复位到达，落点也是"flash 已带 backup_valid=1"的设备——Boot 只要执行新逻辑必搬运
- g_trial_ready 幂等护栏（ota_download.c:535-545）：复位前重试 0x37 直接回正应答、不二次 commit
- 结论：无时序窗口，[EXCLUDED]

### [OBSERVATION] D5-F 仓库 SW_VERSION_STR 仍为 1.1.1（不重开已排除项）

- can_protocol.c:53 `"QC_JYF_FW_1.1.1"`；任务已声明"用户确认 Keil 侧已改 1.1.2"，按指令不重审构建链。
  仅提示重测方法论：**copy 成败不要单凭 0xF195 判定**——若被刷镜像内嵌 1.1.1，
  搬运成功后 DID 仍报 1.1.1；应以 boot 诊断帧 M2 committed + 0x2115=0x03 + 0x2114=0xFE 组合判据

---

## 链路断点定位（一句话）

断点 = **[Boot 执行环节] 设备 Boot ROM 二进制早于 OTA-ARCH-0920 架构重构**（部署态，
非源码逻辑缺陷）；APP 侧 commit_backup（ota_download.c:261）→ ota_metadata_save
（ota_trigger.c:162）全链已证明正确落盘，Boot 侧 main.c:39 判定/copy 状态机
（boot_trial.c:115）已证明逻辑闭合，但**设备上跑的不是这份 Boot**。
仓储证据：Objects/bootloader.bin（2026-09-20 09:23）< 6e9ab04（17:04）。

---

## 修复建议（按优先级）

1. **P0-1（重测前置）** 修复 boot_trial.c:60-75 copy_erase_app_region：
   擦除循环前 `__disable_irq(); flash_unlock();`、循环后 `flash_lock(); __enable_irq();`
   ——对齐 copy_program_region（boot_trial.c:90-91）与 incident 4ec5858 纪律。
   不修则新 Boot 重测必挂在搬运第 2/3 步（fail_step 0xFE/0xFD）。
2. **P0-2（重测前置）** 对齐 M2 诊断帧字节序：boot_safe_mode.h:35-36 文档、
   boot_safe_mode.c boot_diag_m2 实现、zcanpro_boot_diag_capture.py 解码表三处二选一统一
   （建议实现改 p[1]=ret, p[2]=info 以匹配既有文档与解码表）。
3. **P1** 从 7581fed 之后的工作区重新编译 Boot → merge_prod_bin.py 重新合并
   （显式校验 --boot 指向新构建，建议在脚本中加 Boot 制品新鲜度/哈希打印）→
   **SWD/ISP 从 0x08000000 整片烧录**（CAN OTA 无法更新 Boot，必须全片）。
4. **P2（重测验证）** 烧录后先跑 zcanpro_boot_diag_capture.py 抓 Boot 决策帧
   （0x18FF480D，boot_safe_mode.h:32）再跑升级：预期序列 M1（meta_src=0/1, crc_ok=1）→
   M2 start → M3 pre-verify(Backup) → M3 pass → M2 committed → M3(App) pass →
   M4 0x08004100；DID 终判据 0x2115=0x03 ∧ 0x2114=0xFE ∧ 版本号=镜像内嵌值。
5. **P3（防复发）** 增加 Boot 身份可证实性：Boot 构建指纹（git describe/时间戳）写入
   DeviceInfo 或由 Boot 在跳转前经诊断帧 M4 扩展上报；升级脚本在 0x31 前探测并闸门校验；
   zcanpro 判别矩阵补一行："2114=0x02 ∧ 2115=0x00 ∧ 2116=0 ∧ 版本旧 → Boot 陈旧/无 copy 引擎，
   断电重跑无效，需整片重烧 Boot"。
6. **P3** ECDSA 双工程公钥改为构建期同源生成（单一头文件被两工程 include）或加打包期比对；
   zcanpro_ext_ota_auto.py 增加 0xF180 探测（在 Boot 身份可证实性落地前作为弱信号记录）。

---

## 返回结构化摘要

```json
{
  "summary": "APP metadata 写入链与双工程 v3 结构一致性逐环节闭合：0x37→0x77 门禁证明 commit_backup 双副本已回读落盘；Boot 状态机穷举证明 backup_valid=1 时不存在不落盘 0x03/0x04 的路径。实测 DID(0x2114=0x02∧0x2115=0x00∧0x2116=0) 在当前源码 Boot 上不可达 => 断点为部署态：设备 Boot 二进制早于 6e9ab04 架构重构，copy 引擎从未执行。仓储佐证：Objects/bootloader.bin(09-20 09:23) 早于全部重构 commit(17:04-18:46)，merge_prod_bin.py 默认吃该陈旧制品，且 CAN OTA 设计上无法更新 Boot。另发现重测阻断项：copy_erase_app_region 漏 flash_unlock/IRQ 屏蔽（全工程唯一），新 Boot 不修必败。",
  "verdict": "ROOT CAUSE CONFIRMED（部署层断点：设备 Boot 陈旧）+ RETEST BLOCKER（P0：boot_trial.c 擦除缺 unlock）",
  "confidence": 88,
  "break_point": "[Boot 执行环节] 设备 Boot ROM ≠ 当前源码（构建早于 6e9ab04 架构重构）；Boot main.c:39 backup_valid 判定→boot_trial.c:115 copy 状态机从未执行。非 APP 写入断点（ota_download.c:261/ota_trigger.c:162 已证明落盘），非结构不一致断点（两侧 v3 逐字节一致），非 Boot 源码逻辑断点。",
  "fix_suggestion": "①P0 修复 boot_trial.c:60-75 copy_erase_app_region（__disable_irq+flash_unlock，对齐 4ec5858 纪律）；②P0 统一 boot_diag_m2 字节序（头文件/实现/抓帧脚本三处）；③P1 从 7581fed 后源码重编 Boot→merge_prod_bin.py 重合并→SWD/ISP 0x08000000 整片烧录（CAN OTA 不更新 Boot）；④P2 重测用 zcanpro_boot_diag_capture.py 抓 0x18FF480D M1-M4，终判据 0x2115=0x03∧0x2114=0xFE∧M2 committed；⑤P3 增加 Boot 构建指纹可证实性+脚本烧录前闸门+判别矩阵补 stale-Boot 行+ECDSA 双源键构建期同源。"
}
```
