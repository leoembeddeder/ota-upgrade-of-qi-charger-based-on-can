# OTA-ARCH-0920 核心批复审报告（evaluator）

- 任务：OTA-ARCH-0920-EVAL 核心批代码复审（二次审查）
- 基线：origin/main = 6e9ab0462a2295ea641ced17b98de831cf4e344e（11839c8→6e9ab04 单提交，200 文件 +1312/-42723，git fetch/pull 后 HEAD 与任务书一致）
- 范围：核心批（代码+一致性）；docs 批不在范围（不一致记 non_blocking 备忘）
- 执行：evaluator | 派单：龙虾主管 | 2026-09-20 17:08-18:20 GMT+8
- 方法：coder-delivery-review + ota-bootloader-audit 流程；只读审查+独立验证命令；机械枚举项（Keil 工程双向核对/200 文件范围审计）由并行子任务执行后本席复核采纳
- **总体判定：PASS_WITH_RISKS**（blocking=0；non_blocking 8 项，其中 N5 为放行前置条件）

---

## 复审清单逐项结论

### 1. 分区表与地址重叠 — PASS

五处独立 grep 交叉核对（全部 @6e9ab04 实测）：

| 常量 | boot_metadata.h | ota_trigger.h | Python | uvprojx |
|---|---|---|---|---|
| Boot 基址/大小 | :36-37 0x08000000/0x4000 | :33-34 同值 | merge :35-36 同值 | bootloader IROM(0x08000000,0x4000) :21 |
| App 基址/大小 | :38-39 0x08004000/0xC000 | :35-36 同值 | zcanpro :195-196、verify :26-27 同值 | app IROM(0x08004100,0xBF00) :20 |
| App 入口 | :43 APP_BASE+256=0x08004100 | :39 同式 | verify :28 同式 | IROM 起点 0x08004100 ✓ |
| Backup 基址/大小 | :40-41 0x08010000/0xC000 | :37-38 同值 | zcanpro :198-199、verify :29、DOWNLOAD_ADDR :174=0x08010000 | — |
| Metadata 主/备 | :44-45 0x0801C000/0x0801C800 | :41-42 同值 | verify 文档字符串同值 | — |
| DeviceInfo/NVM | :47-48 0x0801D000/0x1000 | 注释引用同值 | — | — |
| 头大小/魔数 | IMAGE_HEADER_SIZE=256 :42；META_MAGIC=0x4F54414D :53 | OTA_IMAGE_HEADER_SIZE=256 :44；OTA_IMAGE_MAGIC=0x4F544158 :49 | IMAGE_HEADER_SIZE=256/IMAGE_MAGIC=0x4F544158 :185-186 | — |

边界算式复算（全部零重叠）：0x08000000+0x4000=0x08004000（Boot 尾=App 头，相接）；0x08004000+0xC000=0x08010000（App 尾=Backup 头）；0x08010000+0xC000=0x0801C000（Backup 尾=Metadata 头）；Backup 末字节 0x0801BFFF<0x0801C000 ✓；Metadata 0x0801C000/0x0801C800 两扇区（272B 结构<0x400 页）+DeviceInfo 0x0801D000+NVM 0x0801E000..0x0801FFFF（=128KB flash 末端）互不重叠 ✓。跳转地址一致性：APP_ENTRY=0x08004100 两工程+Python+Keil IROM 四处一致；app IROM 0x08004100+0xBF00=0x08010000 正好收于 App 代码窗末端（含 XATO 头 256B 后镜像区=0x08004000..0x0800FFFF）✓。coder 自审 §1 表与本席独立 grep 逐项一致，虚报=无。

### 2. metadata v2 持久化 — PASS

**272B 结构两工程逐字节同源（逐字段人工比对）**：

boot_metadata.h :77-93 vs ota_trigger.h :87-103 —— magic@0x00 / version@0x04 / reserved_slots[2]@0x08 / app_valid@0x0A / backup_valid@0x0B / app_crc32@0x0C / backup_crc32@0x10 / reserved_trial[8]@0x14 / copy_retry_count@0x1C / last_boot_reason@0x20 / ota_state@0x21 / reserved2[2]@0x22 / padding[232]@0x24 / crc32@0x10C —— 字段名/顺序/类型/偏移注释完全一致；合计 4+4+2+1+1+4+4+8+4+1+1+2+232+4=272B ✓，crc32@0x10C 双侧一致 ✓。

**v1 拒绝→defaults 重建**：boot_metadata.c meta_validate :100-114 —— magic!=META_MAGIC→-1；**version!=META_VERSION(2)→-1（:104-107）**；crc 校验；boot_metadata_init 读取路径 :93-108=主区→备区→两级全坏→meta_fill_defaults（:117-134，version 置 2）重建落盘 ✓。APP 侧 ota_trigger.c ota_metadata_read 同模式（主→备→defaults :93-108 证据为 boot 侧，app 侧同构声明于 ota_trigger.h:107 + commit_backup 内 ota_metadata_read 失败→memset+magic/version 重建（ota_download.c commit_backup 实证））。

**双副本落盘**：boot 侧 boot_metadata_save —— crc 重算→**备区先写**（meta_write_to_flash(META_BACKUP_ADDR)）→主区后写（:证据函数体）；app 侧 ota_metadata_save —— meta_copy crc→**备区先写**（ota_trigger.c :158 meta_write_to_flash(OTA_META_BACKUP_ADDR,...)）→主区后写。写入咽喉点 meta_write_to_flash 两侧均 __disable_irq+flash_unlock+扇区擦+字编程+flash_lock+__enable_irq（4ec5858 单 bank 擦写事故防护）；boot 侧额外含**逐字读回校验循环**，app 侧无读回（→N1）。APP 置 flag 路径：ota_download.c commit_backup —— meta.backup_valid=1U+meta.backup_crc32=hdr->crc32+ota_state=IDLE→ota_metadata_save ✓。

### 3. 断电幂等链 — PASS（四条全实证）

**① 擦 App 区前必先验 Backup**：boot_trial.c boot_copy_backup —— step1 `boot_verify_image(BACKUP_BASE_ADDR, BACKUP_SIZE, APP_BASE_ADDR, APP_SIZE)`（:124-125，验签+向量须落 App 窗口）失败→fail_step/retry/save→return -1（擦除在 step2 之后，不可达）；另有 staging 一致性闸 `meta->backup_crc32 != hdr->crc32→0xFF 拒擦`。坏镜像不触发擦除 ✓。

**② backup_valid 仅在 App 区复核通过后清除**：step4 `boot_verify_image(APP_BASE_ADDR, APP_SIZE, APP_BASE_ADDR, APP_SIZE)` 通过后才 step5「only now clear the pending flag (power-loss safe)」—— app_valid=1/app_crc32=hdr->crc32/backup_valid=0/ota_state=IDLE→boot_metadata_save ✓。

**③ 任一点断电整链重跑**：main.c step3 :39-53 —— 每次上电 `boot_backup_pending(&g_meta)`（backup_valid!=0）→boot_copy_backup；失败「flag left set→retry next boot；try current App image anyway」（代码+注释实证）；step4 boot_app_image_ok()=0→boot_jump_to_app(APP_ENTRY_ADDR)（:60-62）；无可启动镜像→enter_safe_mode（:66-67，cause 按 fail_step 分流 0x03/0x02）→while(1)。flag 未清期间断电→下轮重跑全链 ✓。

**④ copy 失败 fail_step/copy_retry_count 落盘**：boot_copy_backup 四个失败分支全部——验签失败（fail_step=g_verify_fail_step）/CRC 记录不符（0xFF）/擦除失败（0xFE）/搬运编程失败（0xFD）/App 复核失败（g_verify_fail_step）——每分支 `reserved_trial[META_COPY_FAIL_STEP_OFF]=值 + copy_retry_count++ + last_boot_reason=BOOT_REASON_COPY_FAIL + boot_metadata_save(meta)` 即时落盘（META_COPY_FAIL_STEP_OFF=0 定义于 boot_metadata.h:68）✓。

### 4. C 端静态编译风险 — PASS with notes

**a. 残留符号**：`git grep -E 'ota_trial_init|ota_trial_poll' 6e9ab04 -- '*.c' '*.h'` = **零命中**；`slot_addr|slot_size|select_boot_slot|try_boot_slot|relocate_image` 非注释 C/H 引用 = **零命中**（grep 过滤注释/deprecated 后空）✓。新符号面=boot_backup_pending/boot_copy_backup/boot_app_image_ok（boot_trial.h 声明）+ota_metadata_read/save/ota_crc32（ota_trigger.h:107-109）+deprecated shim ota_running_slot/ota_running_slot_base（:112-113，实现 ota_trigger.c:173-180 返回 0/OTA_APP_BASE_ADDR）。

**b. 外部符号声明完整性**：g_app_ecdsa_pubkey —— 声明 can_protocol.h:209+定义 can_protocol.c:58；DEVICE_INFO_PUBKEY_LEN —— device_info.h:22（消费 ota_download.c:170/:180/:184-186）；lifecycle API —— lifecycle.h:59/68/77/83 完整声明；metadata API —— boot_metadata.h:97-100（boot_metadata_init/save/boot_crc32/boot_crc32_continue）+ota_trigger.h:107-109（ota_metadata_read/save/ota_crc32）；boot_trial.c include 链含 boot_verify.h(:18)+META_COPY_FAIL_STEP_OFF 于 boot_metadata.h:68 可见；ota_download.c include 12 头（ota_download/ota_trigger/can_protocol/can_driver/sit1145/timer_drv/lifecycle/device_info/sha256/uECC/flash/at32 :20-32）覆盖全部跨模块符号 ✓。

**c. boot_verify 四参新签名**：声明 boot_verify.h:47-48 `int8_t boot_verify_image(uint32_t src_base, uint32_t src_size, uint32_t run_base, uint32_t run_size)`；调用点全集 3+1：boot_trial.c:124-125（BACKUP_BASE_ADDR, BACKUP_SIZE, APP_BASE_ADDR, APP_SIZE——Backup 源验向量落 App 窗）/:170-171（APP_BASE_ADDR, APP_SIZE, APP_BASE_ADDR, APP_SIZE）/:195-196（同前者，boot_app_image_ok）+定义 boot_verify.c:80 —— 全部四参，参数语义逐点正确 ✓。

**d. Keil 工程完整性（并行子任务实证，本席复核）**：app 工程 qi_wireless_code_app.uvprojx Files→磁盘正向 **40/40 命中零缺失**；磁盘→uvprojx 反向：89 源文件中 40 注册（39 .c+1 .s），49 未注册=48 .h（Keil 惯例不登记头文件，走 Include Path，非缺陷）+**1 个 .c：libraries/cmsis/device_support/system_at32f422_426.c——已注册副本 mdk_user/Src/system_at32f422_426.c 的冗余并存**（→N2）；bootloader.uvprojx `git diff 11839c8..6e9ab04` 为空（未动 ✓），Files 列表 22/22 全命中，boot_trial/boot_verify/boot_safe_mode/boot_metadata/boot_jump/main 六文件条目在位+磁盘在位 ✓；ScatterFile：双工程分别指向 `.\Objects\qi_wireless_code_app.sct`/`.\Objects\bootloader.sct`，**仓库树内无任何 .sct**（全树 grep 空）——Objects\=Keil 构建产物目录，sct 由 Keil 按 Target 设置自动生成，惯例不入库；coder 自审如实声明"sct 由 Keil 生成"✓ 非交付缺失；IROM 原文：app `<Cpu>IRAM(0x20000000,0x5000) IROM(0x08004100,0xBF00)`（:20，IROM 节点 StartAddress=0x8004100/Size=0xbf00 :251-255）；bootloader `IROM(0x08000000,0x4000)`（:21，:252-256）✓ 与分区表一致、两工程无重叠。

**e. 头文件保护/宏冲突**：IMAGE_HEADER_SIZE 仅 boot_metadata.h:42 定义（app 侧用 OTA_IMAGE_HEADER_SIZE ota_trigger.h:44，前缀隔离）；META_VERSION 仅 boot 侧 / OTA_META_VERSION 仅 app 侧；`git grep -l 'boot_metadata.h' -- 'qi_wireless_code_app/*.c'` 与反向 `ota_trigger.h` × bootloader .c **双向为空**——无任何 TU 跨工程 include 两侧元数据头，无宏冲突面；两侧头文件均有 include guard（__BOOT_METADATA_H/__OTA_TRIGGER_H）✓。

### 5. Python 端逻辑一致性 — PASS

**zcanpro_ext_ota_auto.py @6e9ab04**：DOWNLOAD_ADDR=0x08010000（:174 注释"0x34 target=Backup region (single-App arch)"）；EXPECTED_SW_VERSION="QC_JYF_FW_1.1.2"（:51）/BASELINE="QC_JYF_FW_1.1.1"（:56）；拒闪门保持 fail-closed（:1451-1459 raise 文案含"零业务流量退出，fail-closed"+构建链四步）；判定闭环=①复位后 APP 应答②0xF195==EXPECTED_SW_VERSION（:16）；UDS 流程符号齐备：SID 常量 :179-180（0x10/0x11/0x22/0x27/0x2E/0x31/0x34/0x36/0x37）+Programming/SecurityAccess 流程（:819-873）+0x31 擦除韧性 _erase_with_retry（:961-972，NRC 定性报错）+0x37 后判定（:808-829 补发 11 01 逻辑保留）；宿主机直跑 exit=0（zcanpro=None 路径安全退出）。

**C 端 ota_download.c @6e9ab04**：文件头 :4-13 声明"UDS 0x31/0x34/0x36/0x37 download into the Backup region…commits metadata (backup_valid=1+backup_crc32), responds 0x77, then NVIC_SystemReset. BOOT performs the physical copy"；g_base=OTA_BACKUP_BASE_ADDR（:312/:60 注释"Backup region base"）；0x31 擦除=逐扇区 g_base..g_size（:331）+**活跃区防护 g_base==OTA_APP_BASE_ADDR→NRC CONDITIONS_NOT_CORRECT(0x22)**（:317-319）+越界防护（g_base+g_size>FLASH_PHYSICAL_END 或 >OTA_META_PRIMARY_ADDR→NRC，:322-326）；0x34 门禁 REQUEST_OUT_OF_RANGE NRC（:306）+会话/安全/长度 NRC 家族（:289/:294/:299）；0x37=verify_backup_image（:197-227，magic/length/CRC/ECDSA+reset 向量须落 [OTA_APP_BASE+256, OTA_APP_BASE+OTA_APP_SIZE) 窗口 :225-227）→commit_backup（flag+crc 落盘，§2 已引）→0x77→复位 ✓。Python-C 互证成立：脚本 0x34 目标=0x08010000 ↔ C 端 g_base=OTA_BACKUP_BASE_ADDR 同值；擦除目标/门禁/提交语义一致。

**pack/verify/merge 常量同源**：verify_image.py :23-29（IMAGE_MAGIC=0x4F544158/IMAGE_HEADER_SIZE=256/APP_BASE_ADDR=0x08004000/APP_SIZE=0xC000/APP_ENTRY_ADDR=APP_BASE+256/BACKUP_BASE_ADDR=0x08010000）+reset 向量窗口校验 [0x08004100, 0x08010000)（:5 docstring+:93 verify_reset_handler）；merge_prod_bin.py :35-39（BOOT_BASE=0x08000000/BOOT_SIZE=0x4000/APP_OFFSET=BOOT_SIZE/APP_BASE=BOOT_BASE+APP_OFFSET）——与 C 端五处常量全部同值 ✓。

**EXPECTED=1.1.2 vs C 端 SW_VERSION_STR**：can_protocol.c:53（qi_wireless_code_app）`SW_VERSION_STR[]="QC_JYF_FW_1.1.1"`——仓库固件保持 1.1.1、脚本固化期望 1.1.2，与既有批次口径一致（用户 Rebuild 时改版本号，拒闪门 fail-closed 兜底）→N7 备忘。

**独立复测**：py_compile 5 脚本（ota_auto/capture/pack/verify/merge）**全部 OK**；pyflakes **全部零告警**（/tmp blob+PYTHONPYCACHEPREFIX 重定向，仓库零写入）；zcanpro_boot_diag_capture.py 宿主机自测 **21/21 PASS**（单 App 解码+UDS 基线形态+降级兜底，输出含"读记录=app_valid=过…搬运=成功…实跳=0x08004100(App区)"新语义摘要）——coder"capture 21/21"声称独立复现 ✓。

**DID 语义**：0x2113 恒 0x00=shim ota_running_slot() 返回 0U（ota_trigger.c:178-180）+ota_trigger.h:112 注释"always 0: App region"实证 ✓；0x2114 下载中 0x02/否则 0xFE=OTA_DL_TARGET_BACKUP=0x02U（ota_trigger.h:52，:51 注释"0x2114 during download reports 0x02 = backup"）+OTA_SLOT_NONE=0xFEU（:53）常量级实证 ✓；0x2116=copy_retry_count **行级未钉死**（grep 表达式未命中应答构造行）→N3：语义级证据（常量+shim+capture 解码标签"槽=App区"三方一致）成立，精确应答行待实机/后续复核。

### 6. 诊断帧同步 — PASS

**C 端规格 @6e9ab04**：boot_safe_mode.h :34-39 —— M1 0xA1 [app_valid][meta_src 0=primary/1=backup-copy/2=defaults]…；M2 0xA2 [copy result 0=start-or-none/1=committed/0xFF=failed]；M3 0xA3 [pass][fail_step 0xFF=verifying][target 0=Backup/1=App]；M4 0xA4 [jump addr LE 4B](App entry 0x08004100)；BOOT_DIAG_CAN_ID/0xCC 填充语义保留（:26-28）。实现 boot_safe_mode.c :136-198 —— M1 p[1]=m->app_valid/p[2]=g_diag_meta_src/p[3]=magic_ok/p[4]=ver_ok；**M2 p[1]=info,p[2]=ret（:170-172）**；M3 p[1]=pass/p[2]=fail_step/p[3]=target（:183-186）；M4 p[1..4]=addr LE（:194-198）。

**capture 脚本 @6e9ab04 双向对照**：docstring :32-35 语义表与 C 头一致；解析 :156-169 —— M1 {"app_valid":b[1],"meta_src":b[2],"magic_ok":b[3],"ver_ok":b[4],"crc_ok":b[5]} ↔ C p[1..5] 精确对齐；**M2 {"info":b[1],"ret":b[2]}（:162）↔ C p[1]=info/p[2]=ret 精确对齐**；M3 {"passed":b[1],"fail_step":b[2],"target":b[3]} ↔ C p[1..3] 对齐；M4 addr=b[1]|b[2]<<8|b[3]<<16|b[4]<<24 ↔ C LE 对齐。解码分支 ret==1→"成功（Backup→App 已提交）"/ret==0xFF→"失败 detail=info（flag 保留，下轮开机重试）"/info==0xFF→"无待搬运固件"/else→"搬运序列开始"——与 main.c 发射形态（start m2(0,0)/committed m2(1,0)/failed m2(0xFF,fail_step)）对应正确；INFO：第 4 分支"无待搬运"（info=0xFF,ret=0）当前 C 端无发射点（防御性解码分支，无害）。自测帧 :772-776（M2 copy ok/not pending/copy fail）字节序按 C 形态构造 ✓。**通信层 v2 无回退**：v2 特征标记 7 处在位（response_timeout_ms=2000/trans_stmin=10/enhanced_timeout_ms=8000/transmit(list)/bit31 0x80000000 等 grep 计数=7）；DIAG_CAN_ID=0x18FF480D 未变 ✓。

### 7. git 范围审计 — PASS（并行子任务实证，本席复核）

范围=单提交 6e9ab04（rev-list count=1）；e1c753f 确认为基线 11839c8 的上游祖先，不在本批 ✓。状态统计：**A=3/D=93/R=89/M=15=200** ✓。D 项：90/93 属 qi_wireless_code_slotB/（与"90 删"声称一致）；3 例外=slotA 侧 ota_trigger.h/ota_download.c/ota_trigger.c（重写幅度过大 git 未配成 rename 的配对删除，均在授权 slotA→app 重构路径内）✓。R 项：89 项 dst 全部落于 qi_wireless_code_app/ ✓；src=88 slotA+**1 slotB**（qi_wireless_code_slotB/.../ota_download.c→app/.../ota_download.c——app 的 ota_download.c 源自 slotB 版改写；本席 Item5 已实证其内容为单 App 新实现（g_base=OTA_BACKUP/commit_backup/活跃区防护），非旧 B 槽逻辑残留，来源风险解除）；basename 例外 1=uvprojx 有意改名（qi_wireless.uvprojx→qi_wireless_code_app.uvprojx，提交说明载明 TargetName/OutputName/ScatterFile 同步改名，授权范围内）。M/A 18 项逐项判定全部落在授权类别（5 python_tools+10 bootloader C/H+1 bootloader main.c+2 app 新增 ota_trigger.h/c+1 review_reports 报告），**越权项=无**。embedded_lib diff=0 ✓；全量 200 路径扫描 `../`/绝对盘符(CDEF IJK)/`/mnt/`=**0 命中** ✓；docs/ 零命中（docs 批不在范围 ✓）。计数偏差：coder 自审"90 删/83 rename/91 删除项"vs git 实测 D=93/R=89（口径差异，全部条目已归因）→N4。

### 8. coder 自审报告质量 — PASS（如实度高，2 处表述偏差→N4）

对照复核：§1 分区常量表=本席独立 grep 逐项一致，无虚报；边界算式复算正确；"evaluator 必查四项"（链接地址重叠/标志位持久化/搬运断电恢复/脚本常量一致性）自审结论与本席独立验证结果**全部吻合**；补充自查项（0x34 门禁/活跃区防护 NRC 0x22/判定闭环 fail-closed 保持/诊断 v2 零回退/relocate 系删除）均实证成立；**未验证项如实列报**（MDK 编译/.map IROM 预算/硬件实测/sct 生成）未虚报"已验证"✓。偏差记录：①"backup 先写、primary 后写、**逐字读回校验**"表述过度概括——boot 侧 meta_write_to_flash 有读回循环，app 侧 ota_trigger.c 无（→N1）；②删除/重命名计数与 git 实测偏差（→N4）。均不构成虚报级问题。

---

## 判定汇总

**verdict: PASS_WITH_RISKS**（blocking=0）

### blocking（必改项）
无。

### non_blocking（风险段）
| ID | 严重度 | 内容 | 处置 |
|---|---|---|---|
| N1 | 🟡中低 | app 侧 ota_trigger.c meta_write_to_flash 无逐字读回校验（boot 侧有读回循环）；双副本+备先写结构使 torn write 可被另一副本恢复，风险被结构收容 | 建议后续补齐 app 侧读回对齐 boot 侧（防御性加固，不阻塞本批） |
| N2 | 🟢低 | libraries/cmsis/device_support/system_at32f422_426.c 为已注册 mdk_user/Src 同名文件的未注册冗余副本，存在内容漂移隐患 | 建议核对两副本一致性或移除冗余副本 |
| N3 | 🟢低 | DID 0x2116=copy_retry_count 应答构造行未逐行钉死（0x2113 恒 0x00 已由 shim 实证、0x2114 语义已由常量+注释实证） | 实机 UDS 读值验证；后续复核补钉行号 |
| N4 | 🟢低 | coder 自审计数表述偏差（90/83/91 vs git 实测 D=93/R=89）+"逐字读回校验"过度概括 | 文档精度；docs 批修订时统一口径 |
| N5 | 🟡中 | **C 代码未编译（WSL 无 MDK）——放行前置条件**：boot_trial.c 搬运引擎+ota_download.c 重写首次 Keil Rebuild 可能暴露编译/链接问题；.map IROM 预算（Boot 16KB 内搬运代码增量）未核对 | 用户双工程 Rebuild→.map 核对（bootloader IROM≤0x4000；app IROM1≤0xBF00）→编译结果回报；超限/报错→回 coder 修 |
| N6 | 🟢备忘 | docs 批进行中：docs 2/3/9/10/11/14/16/README/AGENTS/脚本使用说明仍描述 A/B 槽（任务书明示范围外） | docs 批完成前文档-代码不一致为预期过渡态 |
| N7 | 🟢备忘 | EXPECTED_SW_VERSION=1.1.2 vs 仓库 SW_VERSION_STR=1.1.1（can_protocol.c:53）——设计内：用户 Rebuild 时改版本号，拒闪门 fail-closed 兜底 | 沿用既有实测流程口径 |
| N8 | 🟢低 | 仓库遗留构建产物（qi_wireless.bin 旧名，coder 自报）；.sct 不在库（Keil 生成惯例）；python SLOT_A_BASE/SLOT_B_BASE 兼容别名常量保留（=APP_BASE/BACKUP_BASE，非逻辑残留） | docs 批/卫生提交处理 |

### evidence_digest（关键证据）
1. 分区表五处独立 grep 一致+边界算式零重叠：0x08000000+0x4000=0x08004000/+0xC000=0x08010000/+0xC000=0x0801C000；Keil IROM 实测 bootloader(0x08000000,0x4000)/app(0x08004100,0xBF00)，0x08004100+0xBF00=0x08010000 收口。
2. metadata v2：两工程 272B 结构逐字段同序同偏移（crc32@0x10C 双侧一致）；meta_validate version≠2 拒绝→defaults 路径（boot_metadata.c:104-107/:117-134）；boot_metadata_save 备先主后+双侧 __disable_irq 咽喉点实证。
3. 断电幂等链四条全实证：先验后擦（boot_trial.c:124-125 verify BACKUP→CRC 闸→:146 才擦）；复核后清 flag（step4 通过→step5 backup_valid=0+save）；main.c:39-67 每次上电重检 flag→重跑/兜底 jump/safe_mode；四失败分支 fail_step(0xFF/0xFE/0xFD/verify)+copy_retry_count+save 即时落盘。
4. 编译风险静态全过：A/B 时代符号 C/H 零残留（grep 双零）；boot_verify 四参签名 3 调用点+定义全对齐；Keil 工程双向核对 app 40/40+bootloader 22/22 零缺失、uvprojx 未动、IROM 无重叠；无跨工程宏冲突（双向 include grep 空）。
5. Python-C 互证+独立复测：0x34@0x08010000（脚本:174）↔g_base=OTA_BACKUP_BASE_ADDR（C:312）；C 端 0x31 活跃区防护 NRC 0x22（:317-319）+commit_backup flag 路径（backup_valid=1+backup_crc32+save）实证；py_compile+pyflakes 5 文件双零；capture 自测 21/21 独立复现；诊断帧 M2 字节序 C↔脚本精确一致（C p[1]=info/p[2]=ret ↔ 脚本 {"info":b[1],"ret":b[2]}）。

### files_reviewed
深查 22 个：boot_metadata.h/c、boot_trial.c/h、boot_verify.h/c、boot_safe_mode.h/c、boot_jump.c、main.c(bootloader)、ota_trigger.h/c(app)、ota_download.c、can_protocol.c、device_info.h、lifecycle.h、bootloader.uvprojx、qi_wireless_code_app.uvprojx、zcanpro_ext_ota_auto.py、zcanpro_boot_diag_capture.py、verify_image.py、merge_prod_bin.py、OTA-ARCH-0920-core-review.md（另 pack_image.py 常量区抽查）；范围审计覆盖全部 200 变更文件枚举。

### confidence: 88
（8 项清单全带证据闭环；扣分集中于 N3 行级未钉死+N5 编译/硬件不可实测的固有不确定性——后者已列为放行前置条件。）

*—— 评估员已完成审查 | 只读铁律：未修改任何业务代码；本报告为唯一写入产物*
