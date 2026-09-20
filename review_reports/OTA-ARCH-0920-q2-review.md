# OTA-ARCH-0920-Q2 metadata 结构瘦身审计报告

基线：90e1c95（pull 后实际 HEAD）；执行：coder；日期：2026-09-20
用户指令："按照现在的升级逻辑，把 metadata 中没必要的变量都删了吧"
结果：v2(272B) → **v3(28B)**，META_VERSION 2→3（v2 及以前数据校验拒绝→defaults 重建，设备旧 metadata 被重建属预期）

## 一、字段审计表（每字段→保留/删除→依据 file:line）

| 字段 | 判定 | 依据 |
|---|---|---|
| magic | 保留 | 读：boot_metadata.c:100 meta_validate + boot_safe_mode.c:149 M1帧 b3 |
| version | 保留（2→3） | 读：boot_metadata.c:104 + ota_trigger.c:31 meta_validate + boot_safe_mode.c:150 M1帧 b4 |
| reserved_slots[2] | **删除** | 仅 defaults 写（boot_metadata.c:123-124 / ota_trigger.c:50-51），全仓零读 |
| app_valid | 保留 | 读：boot_safe_mode.c:145 M1帧 b1 + capture 解码 zcanpro_boot_diag_capture.py:185/:296（诊断面读消费；Boot 搬运逻辑不读但字段有消费方） |
| backup_valid | 保留 | 读：boot_trial.c:54 boot_backup_pending（升级触发位）+ can_protocol.c:181/:725（CAN 在线判定+DID 0x2114） |
| app_crc32 | **删除** | 仅写不读（boot_trial.c:183 commit 写 + defaults），全仓零读消费 |
| backup_crc32 | 保留 | 读：boot_trial.c:137 搬运前比对 staging 一致性（升级核心逻辑） |
| reserved_trial[8] | **删除数组**；[0] 改具名 copy_fail_step | [0] 有读消费：main.c:51 M2 诊断 detail + main.c:66 enter_safe_mode cause 判定 → 具名保留；[2..4] safe-mode 现场仅写（boot_safe_mode.c:213-215）→ 写点删除，cause/step 仍由 CAN 帧实时携带（M2/M3/ABT/探测应答） |
| copy_retry_count | 保留 | 读：can_protocol.c:741 DID 0x2116 应答 |
| last_boot_reason | 保留 | 读：can_protocol.c:189 CAN 在线判定 + :733 DID 0x2115 应答 |
| ota_state | 保留 | 读：can_protocol.c:177 CAN 在线判定 + :707 DID OTA_STATE 应答 |
| reserved2[2] | **删除** | 仅写不读（boot_safe_mode.c:216-217 现场写 + defaults），零读消费 |
| padding[232] | **删除** | 零消费；布局冻结随 META_VERSION=3 解除 |
| crc32 | 保留 | 读：boot_metadata.c:108 meta_validate + ota_trigger.c 同构；写：:190/:200 落盘重算 |

改名对照：reserved_trial[META_COPY_FAIL_STEP_OFF] → **copy_fail_step**（uint8_t @0x0A）；删除的 reserved 区无具名化必要。

## 二、v3 结构（双工程逐字节同源，diff 验证 IDENTICAL）

```
magic@0x00(4) version@0x04(4) app_valid@0x08(1) backup_valid@0x09(1)
copy_fail_step@0x0A(1) last_boot_reason@0x0B(1) backup_crc32@0x0C(4)
copy_retry_count@0x10(4) ota_state@0x14(1) reserved[3]@0x15 crc32@0x18(4)
总长 28B；META_CRC32_OFFSET=24
```
CRC 读回/双副本落盘/断电幂等链（先验后擦/复核后清 flag/上电重检）零回退：meta_write_to_flash 两侧读回循环（8bbe8d8）原样保留，函数按 sizeof 自动适配 28B。

## 三、联动同步核查

- 诊断帧 M1~M4：载荷字段（app_valid/meta_src/magic/ver/crc、搬运结果、fail_step/目标区、跳转地址）全部保留 → **构造与 capture 解码无需改动**（self-test 21/21 回归通过自证）
- Python：zcanpro/打包脚本不解析 metadata flash，无偏移/常量耦合 → 零改动（grep 实证仅 :1650 文案提 copy_retry，非结构依赖）
- docs/2 §三：metadata 布局描述已重写为 v3
- 打包头（XATO image header）零触碰 ✓

## 四、验证记录

- py_compile 全脚本通过；pyflakes：zcanpro/capture/pack/verify/merge 零告警（sign_seed.py 2 处 unused-import 为本任务前既有问题，未触碰该文件）
- 双工程结构静态对照：字段名/顺序/类型逐行 diff=IDENTICAL；sizeof 等效推算=28B（4+4+1+1+1+1+4+4+1+3+4）
- 删除字段全仓 grep：reserved_trial/reserved_slots/reserved2/META_COPY_FAIL_STEP_OFF/app_crc32 在 C/py 源零残留（仅 3 行过期注释已同步修正）；padding 仅存于图像头上下文（无关）
- capture 自测 21/21 全部通过；git show --stat 范围自查=8 文件（2 头/4 C/docs2/报告）
- 未验证项：MDK 编译（WSL 限制，如实列报）；设备上 v2→v3 defaults 重建行为待实测

## 五、风险

1. 设备现有 v2 metadata（含乙线取证现场）升级后被 defaults 重建——expected by design（版本拒绝机制），但现场排查时 metadata 历史字段（copy_retry 等）归零
2. safe-mode 现场 reserved 区记录随字段删除消失；故障后取证改依赖 CAN 帧（M2/M3/ABT）与 M1 帧现场，flash 侧仅剩 copy_fail_step/last_boot_reason
3. MDK 未编译；首次 Rebuild 若有 sizeof 相关告警回报处理
