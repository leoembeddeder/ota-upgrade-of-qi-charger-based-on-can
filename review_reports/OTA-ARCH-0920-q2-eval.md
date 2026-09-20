# OTA-ARCH-0920-Q2E metadata 瘦身 diff 复审报告（evaluator）

- 基线：HEAD=d3b550d（90e1c95..d3b550d，11 文件 +125/-97）核实一致；只读复审
- **verdict: FAIL** | blocking=1 | confidence=92

## FAIL 项（blocking）

**B1｜ota_image_header_t 定义被误删 → app 工程编译阻断 + 打包头零触碰声称被证伪**
- d3b550d diff：ota_trigger.h 旧结构块整段删除时，**连带删除了 ota_image_header_t typedef**（XATO 镜像头 app 侧类型定义，256B：magic/image_length/crc32/signature[64]/hdr_reserved_ver[16]/build_timestamp/reserved[160]），新内容仅保留 ota_metadata_t v3。
- 证据：`git grep ota_image_header_t HEAD -- '*.h'` = **零命中**（全仓无任何头文件定义该类型）；消费点仍存在：ota_download.c:199 verify_backup_image `const ota_image_header_t *hdr=(const ota_image_header_t*)g_base`、:264 commit_backup 同型——app 工程首次 Keil Rebuild 必报 unknown type name，编译阻断。
- 越界定性：打包头（XATO image header）零触碰声称不成立；类型删除超出 metadata 瘦身授权范围。boot 侧 ota_image_view_t（boot_verify.h:29）完好不受影响。
- **修复指引（回 coder）**：在 qi_wireless_code_app/mdk_app/Inc/ota_trigger.h（或 ota_download.h）恢复 ota_image_header_t typedef，字段/顺序/类型与删除前逐字一致（对照 90e1c95:ota_trigger.h 同名 struct，256B）；不动 metadata v3 结构。放行判据：全仓 grep 该类型定义点=1、消费点=2（ota_download.c:199/:264）；双工程 Rebuild 通过。可顺手修正自审报告两处偏差（见 notes）。

## 其余清单结论（PASS）

1. **字段审计独立复核 PASS**：删除字段全仓零读残留——reserved_trial/META_COPY_FAIL_STEP_OFF/reserved_slots=0 命中；reserved2[ 仅命中厂商寄存器头（at32f422_426_crm.h:821/flash.h:367，与 metadata 无关）；**app_crc32 全仓（C/h/py）零命中** ✓ 删除安全。保留字段读消费全部实证无反例：backup_valid（boot_trial.c:54 boot_backup_pending+can_protocol.c:181/:725 DID 0x2114）、backup_crc32（boot_trial.c:137 搬运前比对）、copy_retry_count（can_protocol.c:741 DID 0x2116）、last_boot_reason（:189/:733 DID 0x2115）、ota_state（:177/:707）、magic/version/crc32（meta_validate 双侧）。
2. **v3 结构同源 PASS**：boot_metadata.h vs ota_trigger.h 逐字段一致（magic@0x00/version@0x04/app_valid@0x08/backup_valid@0x09/copy_fail_step@0x0A/last_boot_reason@0x0B/backup_crc32@0x0C/copy_retry_count@0x10/ota_state@0x14/reserved[3]@0x15/crc32@0x18）；sizeof=4+4+1+1+1+1+4+4+1+3+4=**28B** ✓；crc32@0x18 双侧 ✓；META_VERSION=3/OTA_META_VERSION=3 双侧 ✓；META_CRC32_OFFSET=(sizeof-4)=24 双侧均为表达式（自动适配）✓；meta_validate version≠META_VERSION(3) 拒绝→defaults 重建路径双侧完好（函数未动，defaults 已更新为 v3 字段）✓ v2 旧数据重建机制成立。
3. **红线零回退 PASS**：①读回校验双侧完好——boot_metadata.c:81 与 ota_trigger.c:149 `*(volatile uint32_t*)(addr+i*4)!=src[i]` 原样，写入范围随 sizeof 自动适配 28B；双副本落盘+IRQ-off 咽喉点未动；②断电幂等链零回退——boot_trial.c 先验后擦（:122-127 verify→:146 擦除顺序未动）/复核后清 flag（:182-188 commit 仅删 app_crc32 写行，app_valid/backup_valid/save 逻辑原样）/main.c 上电重检（:39-53 循环未动，仅字段改名）；③搬运流程逻辑不变（copy_erase/copy_program/total=IMAGE_HEADER_SIZE+hdr->image_length 均未动）。
4. **copy_fail_step 改名 PASS**：reserved_trial/META_COPY_FAIL_STEP_OFF 全仓零残留；写点 boot_trial.c 6 处全在（:127 verify失败/:139 CRC不符0xFF/:149 擦除0xFE/:161 搬运0xFD/:173 复核失败/:186 成功清0）+读点 main.c:51（M2 诊断 detail）/:66（enter_safe_mode cause 0x03/0x02 判定）全在且语义自洽；boot_safe_mode.c 编译面干净——enter_safe_mode 内 step 变量仍被消费（:18 safe_heartbeat(cause,step)/:63 r[4]=step 探测应答/:80 心跳），无未用变量/无残留引用。
5. **联动核查（除 B1 外）PASS**：诊断帧 M1~M4 载荷构造 vs capture 解码一致——M1 用保留字段（app_valid/magic/version+CRC 随 sizeof 自适应），capture **零改动成立**（11 文件不含该脚本）+自测独立复跑 **21/21 PASS**+pyflakes 零告警；DID 映射 v3 完好：0x2114=backup_valid（can_protocol.c:725）、0x2115=last_boot_reason（:733）、0x2116=copy_retry_count（:741）、0x2113 恒 0x00（shim 不依赖 metadata 字段）；python 全脚本 metadata 偏移耦合 grep=空 ✓；docs/2 §三 v3 布局（:49 28B/:60 copy_fail_step@0x0A/:66 crc32@0x18 OFFSET=24）与代码逐项一致。
6. **范围审计**：11 文件=docs2/boot_metadata.h/boot_safe_mode.h/boot_trial.h/boot_metadata.c/boot_safe_mode.c/boot_trial.c/main.c/ota_trigger.h/ota_trigger.c/q2-review.md——除 B1（ota_image_header_t 越界删除）外全部在 Q2 授权范围。

## notes
- coder 自审偏差 2 处：①"范围自查=8 文件"vs 实测 11（漏计 2 头文件注释同步+报告本身）；②"打包头零触碰 ✓"被证伪（见 B1）——其余声称（字段审计表/结构同源/红线零回退/capture 21/21/pyflakes 零告警/未验证项如实列报）与本席独立验证一致。
- 设备侧影响（coder 风险自报确认）：v2 metadata 升级后被 defaults 重建=版本拒绝机制预期行为；safe-mode flash 现场记录随 reserved 区删除消失，取证改依赖 CAN 帧（M2/M3/ABT）+copy_fail_step/last_boot_reason——设计权衡，非缺陷。
- N5 前置沿用：MDK 未编译（WSL 限制）；B1 修复后仍需双工程完整 Rebuild。

## 补充核查（主管 18:34 增补）：v2 陈旧注释全清单（注释修正级，非阻断）

主管发现 boot_metadata.h 头部注释与 v3 代码不符——确认属实并全仓扩展枚举 @d3b550d，共 **7 处/4 文件**：

| # | 文件:行 | 陈旧表述 | 应改为 |
|---|---|---|---|
| 1 | qi_wireless_bootloader/mdk_app/Inc/boot_metadata.h:20 | "byte-frozen (272 bytes, crc32 at offset 268)" | v3：28B，crc32@0x18(24) |
| 2 | 同上 :22 | "META_VERSION bumped to 2: v1 …rejected -> defaults" | META_VERSION=3：v2 及以前拒绝→defaults |
| 3 | 同上 :71（struct 文档注释） | "OTA metadata structure (272 bytes total, layout byte-frozen)" | 28B，布局随版本管理 |
| 4 | qi_wireless_code_app/mdk_app/Inc/ota_trigger.h:18-19 | "byte-frozen (272B, crc32 @0x10C)…legacy slot fields retained as reserved bytes" | 28B/@0x18；legacy 字段已删除非保留 |
| 5 | qi_wireless_bootloader/mdk_app/Src/boot_metadata.c:8（文件头注释） | "(META_VERSION=2): legacy A/B-era metadata is rejected" | META_VERSION=3 |
| 6 | docs/2. Flash 分配方案.md:15（分区表） | "OTA 元数据主副本（272B 结构）" | 28B |
| 7 | docs/2. Flash 分配方案.md:30（ASCII 图） | "ota_metadata_t (272B)" | 28B |

排除项：ota_trigger.h:70 XATO image header "256B, byte-frozen"——打包头确为 256B 未变，表述准确非陈旧；sha256.c:33 常量内含"272"子串——误命中。boot_trial.h/boot_safe_mode.h 注释已随 d3b550d 同步（copy_fail_step 语义），无残留。

处置：与 B1（ota_image_header_t 恢复）一并交 coder 热修或下一批修复；docs/2 §一/§三自相矛盾（§三已改 v3 而 :15/:30 仍 272B）优先修正。注释级问题不影响代码事实（v3 结构/版本/CRC offset 双工程代码层已验证正确），不改变 FAIL verdict（B1 仍为唯一 blocking）。

*—— 评估员已完成复审（含主管增补核查）| 只读铁律：业务代码零触碰*
