# OTA-ARCH-0920 核心批自审报告

任务：OTA 升级架构重构——A/B 双槽 → Boot + App + 备份区（单 App 升级流）
批次：核心批（代码 + 一致性 + 本报告）；docs 批随后单独提交
日期：2026-09-20 | 执行：coder | 派单：龙虾主管

## 1. 新 Flash 分区表（三方同源对照）

| 区域 | 地址范围 | 大小 | Boot C（boot_metadata.h） | APP C（ota_trigger.h） | Python（zcanpro/verify/merge） |
|---|---|---|---|---|---|
| Bootloader | 0x08000000..0x08003FFF | 16KB | BOOT_BASE_ADDR=0x08000000 / BOOT_SIZE=0x4000 | OTA_BOOT_BASE_ADDR / OTA_BOOT_SIZE 同值 | merge_prod_bin.py BOOT_SIZE=0x4000 |
| App（镜像区） | 0x08004000..0x0800FFFF | 48KB | APP_BASE_ADDR=0x08004000 / APP_SIZE=0xC000 | OTA_APP_BASE_ADDR / OTA_APP_SIZE 同值 | APP_BASE=0x08004000 / APP_SIZE=0xC000（zcanpro、verify_image、merge 全同值） |
| App 代码入口 | 0x08004100 | — | APP_ENTRY_ADDR=APP_BASE+256 | OTA_APP_ENTRY_ADDR 同式 | APP_ENTRY=0x08004100；Keil IROM1=0x08004100,0xBF00（uvprojx 实测） |
| Backup（暂存区） | 0x08010000..0x0801BFFF | 48KB | BACKUP_BASE_ADDR=0x08010000 / BACKUP_SIZE=0xC000 | OTA_BACKUP_BASE_ADDR / OTA_BACKUP_SIZE 同值 | BACKUP_BASE=0x08010000 / BACKUP_SIZE=0xC000；DOWNLOAD_ADDR=0x08010000 |
| Metadata 主/备 | 0x0801C000 / 0x0801C800 | 1KB×2 | META_PRIMARY_ADDR / META_BACKUP_ADDR | OTA_META_PRIMARY_ADDR / OTA_META_BACKUP_ADDR 同值 | verify/merge 文档字符串引用同值 |
| DeviceInfo | 0x0801D000 | 4KB | DEVICE_INFO_ADDR | （APP 侧不引用） | — |
| NVM | 0x0801E000..0x0801FFFF | 8KB | （文档层） | — | — |
| XATO 镜像头 | 256B | — | IMAGE_HEADER_SIZE=256；magic=0x4F544158 | OTA_IMAGE_HEADER_SIZE=256；OTA_IMAGE_MAGIC 同值 | IMAGE_HEADER_SIZE=256；IMAGE_MAGIC=0x4F544158 |
| Metadata 版本 | — | — | META_VERSION=2（单 App 格式，v1 拒绝→defaults） | OTA_META_VERSION=2 | — |

Keil 工程 IROM 实测（uvprojx <Cpu> 行）：
- bootloader.uvprojx：IROM(0x08000000,0x4000)——沿用现有地址，与 App 区 0x08004000 起点无重叠（Boot 末端=App 起点，边界相接）✓
- qi_wireless_code_app.uvprojx（原 slotA 工程改名）：IROM(0x08004100,0xBF00)——代码区 0x08004100..0x0800FFFF，含 XATO 头后镜像区=0x08004000..0x0800FFFF ✓ 与新分区表一致，无需调整

地址重叠自查（evaluator 必查项）：0x08000000+0x4000=0x08004000（Boot 尾=App 头，无重叠）；0x08004000+0xC000=0x08010000（App 尾=Backup 头，无重叠）；0x08010000+0xC000=0x0801C000（Backup 尾=Metadata 头，无重叠）；Backup 0x0801BFFF < Metadata 0x0801C000 ✓。全部区域边界相接零重叠。

## 2. 升级流程与分工（代码现状）

1. 上位机（zcanpro_ext_ota_auto.py）：UDS 10 02→27→31（擦 Backup 区 0x08010000~0x0801BFFF）→0x34（地址门禁=仅接受 0x08010000）→0x36 流式写入→0x37。
2. APP（ota_download.c ota_dl_poll）：flush→长度核对→verify_backup_image（magic/length/CRC/ECDSA + Reset 向量必须落 App 窗口 [0x08004100,0x08010000)）→commit_backup（metadata: backup_valid=1 + backup_crc32=payload CRC，先备后主双副本落盘）→0x77 正响应→wait_tx_idle→SHUTDOWN→NVIC_SystemReset。APP 从不擦写自身所在 App 区。
3. BOOT（boot_trial.c boot_copy_backup + main.c）：上电读 metadata→backup_valid=1→验 Backup 镜像（同判据再跑一遍）→擦 App 区（48 扇区）→字搬运+读回复核→App 区再验签→**复核通过才清 flag**（app_valid=1/app_crc32/backup_valid=0 落盘）→boot_jump_to_app(0x08004100)。flag 未清之前任何断电，下次上电整链重跑（幂等恢复）。
4. metadata 结构（两工程逐字节同布局 272B，crc32@0x10C）：trial 字段废弃→reserved_trial[8]；slot 字段废弃→reserved_slots[2]；新语义=app_valid/app_crc32/backup_valid(搬运标志)/backup_crc32/copy_retry_count（诊断）。META_VERSION=2：v1 时代 metadata 校验拒绝→defaults 重建，防旧 A/B 数据误触发搬运。

## 3. evaluator 视角自审（任务书必查四项）

| 检查项 | 结论 | 证据 |
|---|---|---|
| 链接地址头尾重叠 | 通过 | §1 边界算式：四区边界相接零重叠；Keil IROM 实测值与分区表一致 |
| 标志位持久化 | 通过 | backup_valid/backup_crc32 存于双副本 metadata（backup 先写、primary 后写；双侧 meta_write_to_flash 均含逐字读回校验——boot 侧原有、app 侧 N1 收尾补齐同构循环（本报告初版表述为时过早，已按落地事实修正）；咽喉点关中断保护）；APP 0x37 只在 verify 全过后置 flag；BOOT 只在 App 区复核过后清 flag |
| 搬运中断电恢复 | 通过 | 擦 App 区前已完成 Backup 验签（坏镜像不会触发擦除）；擦/搬/复核任一点断电→flag 仍在→下次上电重跑全链（Boot 每次上电检测）；copy 失败时 fail_step 记入 reserved_trial[0]、copy_retry_count++ 落盘、M2 诊断帧报告，旧 App 若仍可验签则照常启动（flag 保留下轮再试） |
| 脚本常量一致性 | 通过 | §1 三方对照表 grep 实测一致；zcanpro import 断言 APP_BASE/BACKUP_BASE/DOWNLOAD_ADDR/EXPECTED_SW_VERSION 值正确；pack/verify/merge 常量同源 |

补充自查：0x34 地址门禁（ota_dl_handle_request_download：dl_addr!=g_base 回 NRC 0x31）；擦除活跃区防护（g_base==OTA_APP_BASE_ADDR 回 NRC 0x22）；判定闭环 fail-closed 保持（EXPECTED_SW_VERSION 拒闪门零改动；升级后判定=APP 应答+0xF195==EXPECTED，0x2113 单 App 架构恒 0x00 仅信息展示）；诊断通信层 v2（c64aca3）零回退（仅解码表/标签随架构语义更新，can_send/can_recv/uds 三模式照抄逻辑原样）。

## 4. 关键决策记录

1. 分工定案（任务书默认方案，依代码现状确认）：APP 收→Backup→校验→置 flag→复位；BOOT 检 flag→擦 App→搬运→复核→清 flag→跳转。依据：APP 运行中擦写自身 Flash 区会锁死（单 Bank 取指踩擦写区，4ec5858 事故同根因）。
2. metadata 字节布局冻结 + META_VERSION=2：272B 布局两工程逐字节一致，旧字段保留为 reserved 字节；版本升 2 拒绝 v1 数据，防旧 A/B 时代 metadata 误触发搬运。
3. e1c753f（用户 15:05 双 Target 脚本提交）slotB 逻辑废弃：任务书明令授权（"本次改造将其 slotB 逻辑废弃，属用户明令"）；pack_image.py --slot 参数、ota_auto 槽选择/免重定位（_pick_firmware_for_slot/relocate_image_to_slot/image_target_slot）已删，单 App 版本匹配优先选 bin 逻辑保留。
4. Keil 工程改名：slotA→qi_wireless_code_app（git mv 保留历史），uvprojx TargetName/OutputName/ScatterFile 同步改名，IROM1=0x08004100,0xBF00 与新分区表本已一致无需调整；bootloader 工程 IROM 沿用不动。
5. 诊断帧语义更新（boot_safe_mode.c/h + zcanpro_boot_diag_capture.py 两侧同步）：M1=b1 app_valid；M2=搬运结果(0 序列开始/1 提交成功/0xFF 失败+detail)；M3=验签目标(0=Backup 源/1=App 区)+fail_step；M4=跳转地址(App 0x08004100)。通信层 v2 与 BOOT_DIAG_CAN_ID 不动。
6. can_protocol.c 兼容面：DID 0x2113 恒答 0x00（App 区运行）、0x2114 下载中答 0x02（Backup 标记）否则 0xFE、0x2116=copy_retry_count；ota_running_slot/ota_running_slot_base 保留为 deprecated shim，避免历史主机工具硬失败。
7. docs/4 判断：0xCC IAP 帧层属 Qi 芯片侧 IAP（UART 转发链，AT32 不解析该层），与 AT32 Flash 分区无关——不动；依据=docs/4 描述的是 Qi 芯片 IAP 数据协议（log1/log2 固件经 MCU UART 转发），本仓 AT32 侧 UDS 31/34/36/37 协议在 can_protocol.h/ota_download.c 定义，两层互不引用。

## 5. 验证记录

已验证（本地可执行项）：
- py_compile 通过：zcanpro_ext_ota_auto.py / zcanpro_boot_diag_capture.py / pack_image.py / verify_image.py / merge_prod_bin.py
- pyflakes 零告警（同上 5 文件）
- zcanpro import 断言：APP_BASE=0x08004000 / BACKUP_BASE=0x08010000 / DOWNLOAD_ADDR=0x08010000 / DL_TARGET_BACKUP=2 / EXPECTED=QC_JYF_FW_1.1.2 / BASELINE=QC_JYF_FW_1.1.1 / KEIL_APP_BIN 指向 qi_wireless_code_app 路径；relocate_image_to_slot/_pick_firmware_for_slot/image_target_slot 确认已删除
- 四断言保持：判定闭环/EXPECTED_SW_VERSION/[自检]/11 01（非 suppress 在升级脚本内齐在
- capture 脚本合成流自测 21/21 全部通过（单 App 解码 + UDS 基线形态 + 降级兜底）
- C/Python 分区常量三方 grep 一致（§1 表）
- 遗留槽语义 grep：C 源非注释项 CLEAN；Python 非注释项 CLEAN
- uvprojx XML：app 工程 TargetName/OutputName/ScatterFile=qi_wireless_code_app，IROM(0x08004100,0xBF00)，slotA/slotB 引用计数=0；bootloader uvprojx 未动（IROM 0x08000000,0x4000）
- git 状态实测（evaluator 复核 @735fd30）：A=3/D=93/R=89/M=15=200；D=93=90 slotB 删除+3 slotA 侧重写幅度过大未配 rename 的配对删除；R=89 目标路径全部落 qi_wireless_code_app/；全部条目在任务授权范围内（初版口头计数 91/83 为状态快照口径，以实测为准）

未验证项（WSL 无法执行，如实列报）：
- MDK 编译：bootloader 工程与 qi_wireless_code_app 工程均未在 Keil 下编译（WSL 无 MDK）；C 端改造（boot_metadata/boot_trial/boot_verify/boot_jump/main/boot_safe_mode/ota_trigger/ota_download/can_protocol）仅有静态自查
- .map IROM 大小核对（Boot 侧新增搬运代码后的 16KB 预算）
- 硬件实测：完整 OTA 流程（写 Backup→0x37 提交→Boot 搬运→版本核验）、搬运中断电恢复实测、诊断帧 M1~M4 实机抓取
- Keil 重建后 ScatterFile（qi_wireless_code_app.sct）自动生成一致性（工程文件已指向新名，sct 由 Keil 生成）

## 6. 风险清单

1. C 代码未编译：最大风险项。搬运引擎（boot_trial.c 全新实现）与 ota_download.c 重写若存在编译错误/链接遗漏（如 boot_trial.h 原型与 main.c 调用不匹配、g_app_ecdsa_pubkey/DEVICE_INFO_PUBKEY_LEN 等外部符号声明位置），需用户首次 Keil Rebuild 时暴露并回报。
2. docs 批未完成：docs/2/3/9/10/11/14/16/README/AGENTS.md 与脚本使用说明.md 仍描述 A/B 槽（本批按裁定未动），docs 批单独提交前文档与代码不一致。
3. 仓库内遗留构建产物：qi_wireless_code_app/mdk_project/Objects/ 下历史 qi_wireless.bin（旧名）仍被 git 跟踪；Keil 重建将生成 qi_wireless_code_app.bin，旧名产物成为陈旧文件（不影响编译，docs 批/后续卫生提交处理）。
4. DID 语义降级：0x2113 恒 0x00、0x2114=0x02/0xFE——历史判读矩阵（slot 判定）对新固件失效，现场排障需按新语义（0xF195 版本 + M1~M4 诊断帧 + copy_retry）。
5. e1c753f 废弃授权：用户 15:05 双 Target 逻辑（slotB 打包/槽选择免重定位）随本批删除，依据任务书授权原文；若用户现场仍持有 slotB 依赖流程需知悉已不可用。
6. 私钥路径/产线链：pack_image 默认输入改为 qi_wireless_code_app/.../qi_wireless_code_app.bin（Keil 新输出名）；产线若沿用旧路径脚本将报"skip (not found)"，需按新命名构建。

---
自查结论：本地可验证项全部通过；核心批可提交。C 编译与硬件实测列为未验证项，待用户 Keil Rebuild+实测回报。
