# OTA-ARCH-0920-D4 打包脚本与固件 XATO 包头一致性审计

- 日期：2026-09-21（审计执行 04:22–04:48 CST）
- 范围：打包/校验/合并 Python 脚本 ↔ App/Boot 固件代码的 XATO 镜像头与 metadata v3 布局一致性
- 方法：静态逐字段比对 + **执行级验证**（合成载荷 roundtrip、openssl 双向实验、merge 正/负路径）
- 约束：只读审计，零改码；测试产物仅落 /tmp 与本报告
- **结论：PASS_WITH_RISKS（置信度 92）**
  - 打包字节链路（固件结构 ↔ pack_image_if_needed ↔ verify/merge）**逐字段、逐偏移一致**，"包头数据优化"四个提交与打包脚本完全匹配；
  - 唯一 MISMATCH：`verify_image.py` 的 `--key` ECDSA 辅助函数实现缺陷（宿主工具**假阴性 FAIL**），不影响打包字节布局与固件验证路径，可独立修复后重测该工具。

被审计提交（独立验证均在 origin/main）：`dad004a`（version 区固定 0x00）、`fad6f77`（image_header_t 删 version 字段→hdr_reserved_ver[16]，头 256B 与其余偏移逐字节不变）、`d3b550d`（metadata v3 28B，META_VERSION 2→3）、`aaa7d91`（恢复 ota_image_header_t typedef）。

---

## A. XATO 镜像头逐层一致性（核心）

### A.0 期望布局（审计基准）

| 偏移 | 字段 | 期望 |
|---|---|---|
| 0x00 | magic | 0x4F544158 "XATO"（LE 字节 58 41 54 4F） |
| 0x04 | image_length | payload 字节数 |
| 0x08 | crc32 | payload CRC32 |
| 0x0C | signature[64] | ECDSA P-256 R\|\|S（只覆盖 payload） |
| 0x4C | hdr_reserved_ver[16] | 保留占位，填 0x00 |
| 0x5C | build_timestamp | Unix 时间戳（uint32 LE） |
| 0x60 | reserved[160] | 补齐 256B |
| — | 总大小 | 256B |

### A.1 五层逐一比对结果

| 层 | 位置 | 结论 | 证据 |
|---|---|---|---|
| 1. App 侧 C 结构 | `qi_wireless_code_app/mdk_app/Inc/ota_trigger.h:72-82` | **[MATCH]** | `ota_image_header_t`：magic/ image_length/ crc32/ signature[64]/ hdr_reserved_ver[16]/ build_timestamp/ reserved[160]；4+4+4+64+16+4+160=256，偏移与 A.0 逐项吻合。`OTA_IMAGE_HEADER_SIZE 256U`（:44）、`OTA_IMAGE_MAGIC 0x4F544158U`（:49）。typedef 已恢复（aaa7d91），`ota_download.c:199` 等使用点正常。 |
| 2. Boot 侧 C 结构 | `qi_wireless_bootloader/mdk_app/Inc/boot_verify.h:20-29` | **[MATCH]** | `ota_image_view_t` 与 App 侧字段名/顺序/尺寸**完全同构**（含 `hdr_reserved_ver[16]` @0x4C 注释 "filled 0x00"）。`boot_verify.c` 只消费 magic(:91)、image_length(:99)、crc32(:127)、signature(:182 uECC_verify)、reset vec = `*(src+256+4)`(:137-141)，**不读 0x4C/version** —— 与"version 不再入头"的设计一致。 |
| 3. 打包实现 | `python_tools/zcanpro_ext_ota_auto.py:688-708` `pack_image_if_needed` | **[MATCH]**（执行级证明） | :705 `struct.pack("<III", IMAGE_MAGIC, len(data), crc)` + `sig`(64B) + `hdr_reserved_ver`(16×0x00, :704) + `struct.pack("<I", int(time.time()))`，:706 `b"\x00"*(256-len(header))` 补齐。**实测 dump**（E3 roundtrip）：@0x00=0x4F544158 ✓ @0x04=108=payload len ✓ @0x08=0x28F69E05=zlib.crc32(payload) ✓ @0x0C..0x4B=64B R\|\|S ✓ @0x4C..0x5B=**16×0x00** ✓ @0x5C=unix ts（delta=0）✓ @0x60..0xFF=**160×0x00** ✓ 头总长=**256** ✓。签名域=payload only（`ecdsa_sign_msg` :402-416 对 `data` 即无头固件 SHA256），与固件侧一致。 |
| 4. 校验脚本 | `python_tools/1.packaging script/verify_image.py:23-36,38-52,139-171` | **[MATCH]**（结构）+ **[MISMATCH]**（--key 辅助函数，见 §F-M1） | 常量 :23-36 `IMAGE_MAGIC=0x4F544158 / IMAGE_HEADER_SIZE=256 / HDR_MAGIC_OFF=0x00 / HDR_LEN_OFF=0x04 / HDR_CRC_OFF=0x08 / HDR_SIG_OFF=0x0C / HDR_RESERVED_VER_OFF=0x4C / HDR_BUILD_TS_OFF=0x5C` 全部与 A.0 一致；magic/length/CRC/reset-window 检查逻辑正确（:38-52,:93-100）；@0x4C 仅打印不过闸（:146,:162）。**实测**（E4/E6）：无 --key 时结构五项全 OK → PASS rc=0；`--key` 走 openssl 时假阴性 FAIL（缺陷细节与正确用法对照见 §E-E7、§F-M1）。 |
| 5. 合并脚本 | `python_tools/1.packaging script/merge_prod_bin.py:35-39,98-102` | **[MATCH]**（字节）+ **[RISK-R2]**（warn-only 闸门） | :98 `app_data[:4] != b"XATO"` —— 0x4F544158 LE 字节 = `58 41 54 4F` = "XATO"，字节序正确；APP_BASE = 0x08000000+0x4000 = 0x08004000（:35,:39）✓。**实测**（E8）：合并产物 @0x4000 起首 4 字节=`b'XATO'`，boot 区尾部 0xFF 填充 ✓，布局打印地址正确。负向（E9）：裸 bin 触发 WARNING 但**仍合并 rc=0**（warn-only，见 R2）。 |

### A.2 "包头优化"提交 ↔ 打包脚本对应关系

| 提交 | 声称 | 固件侧证据 | 打包/校验侧证据 | 结论 |
|---|---|---|---|---|
| dad004a | version 区固定填 0x00，版本号唯一定义在 SW_VERSION_STR | `can_protocol.c:53` `SW_VERSION_STR="QC_JYF_FW_1.1.1"` + :669-673 DID 0xF195 只读该常量；`ota_download.c`/`boot_verify.c` 均不读镜像头 version | `zcanpro_ext_ota_auto.py:704` 打包 16×0x00；:707 日志明示"版本号不在镜像头"；OTA 判定改读 DID 0xF195（:1126-1135）；`pack_image.py:10-12` docstring 同步 | **[MATCH]** |
| fad6f77 | 删 version 字段→hdr_reserved_ver[16]，256B 头与其余偏移逐字节不变 | `ota_trigger.h:80` `hdr_reserved_ver[16]`@0x4C；`boot_verify.h:26` 同源同位；build_timestamp 仍 @0x5C、reserved[160]@0x60 | `zcanpro_ext_ota_auto.py:190-192` `HDR_RESERVED_VER_OFF=0x4C / LEN=16 / HDR_BUILD_TS_OFF=0x5C`（注释"偏移锁定不可回收"）；`verify_image.py:35-36` 同值 | **[MATCH]** |
| d3b550d | metadata 瘦身 v3，28B，META_VERSION 2→3 | `boot_metadata.h:76-89` 与 `ota_trigger.h:87-101` 逐字段同构 28B，crc32@0x18；`boot_metadata.h:55`/`ota_trigger.h:48` 版本=3；双侧 `version != 3 → return -1`（`ota_trigger.c:31`、`boot_metadata.c:105`）→ v2 拒绝重建默认 | Python 打包/OTA 脚本**零 metadata 字节布局常量**（§C），瘦身无外部耦合面 | **[MATCH]** |
| aaa7d91 | 恢复 ota_image_header_t typedef | `ota_trigger.h:72-82` typedef 在位；`ota_download.c:199,264` 引用正常 | 不涉及 Python 侧（Python 用字节偏移常量，不用 C typedef） | **[MATCH]** |

---

## B. 常量一致性矩阵

| 常量 | Boot C | App C | zcanpro_ext_ota_auto.py | verify_image.py | merge_prod_bin.py | 结论 |
|---|---|---|---|---|---|---|
| 头大小=256 | `boot_metadata.h:43` 256U | `ota_trigger.h:44` 256U | :186 `256` | :24 `256` | n/a（不涉头长） | **[MATCH]** |
| magic=0x4F544158 | `boot_verify.c:91` 字面量 `0x4F544158U`（boot 工程无宏，见 R4） | `ota_trigger.h:49` 宏 | :185 | :23 | :98 `b"XATO"`（LE 字节等价） | **[MATCH]** |
| APP_BASE=0x08004000 | `boot_metadata.h:39` | `ota_trigger.h:35` | :195 | :26 | :39 计算值 | **[MATCH]** |
| APP_ENTRY=0x08004100 | `boot_metadata.h:44`（base+256 计算） | `ota_trigger.h:39`（计算；宏前向引用合法，展开在使用点） | :197 显式 | :28 计算 | n/a | **[MATCH]** |
| APP_SIZE=0xC000 | `boot_metadata.h:40` | `ota_trigger.h:36` | :196 | :27 | :37 | **[MATCH]** |
| BACKUP=0x08010000 | `boot_metadata.h:41` | `ota_trigger.h:37` | :198（+ :174 `DOWNLOAD_ADDR` 同值） | :29 | :7 文档串 | **[MATCH]** |
| BOOT_BASE/BOOT_SIZE=0x08000000/0x4000 | `boot_metadata.h:37-38` | `ota_trigger.h:33-34` | n/a | n/a | :35-36 | **[MATCH]** |
| META_MAGIC=0x4F54414D | `boot_metadata.h:54` | `ota_trigger.h:47` | 无（不建 metadata 字节） | 无 | 无（:8 仅文档） | **[MATCH]**（Python 无耦合面，[EXCLUDED]） |
| META_VERSION=3 | `boot_metadata.h:55` | `ota_trigger.h:48` | 无 | 无 | 无 | **[MATCH]**（同上） |
| META 主/备区=0x0801C000/0x0801C800 | `boot_metadata.h:45-46` | `ota_trigger.h:40-41` | 无 | 无 | :117 打印 0x0801C000 | **[MATCH]**；诊断脚本 `zcanpro_boot_diag_capture.py:66-68` META_SRC 地址同值 |

---

## C. metadata v3 与 Python 脚本耦合审计

**结论：无字节布局耦合 —— v3 瘦身对 Python 侧零冲击 [MATCH]。**

1. **打包/OTA 主链路不碰 metadata**：`pack_image.py`、`verify_image.py`、`merge_prod_bin.py`、`zcanpro_ext_ota_auto.py` 全仓 grep `META/MATO/ota_state/backup_valid/app_valid/0x4F54414D/0x0801C000/0x0801C800` —— 仅命中：注释/日志文案（`zcanpro_ext_ota_auto.py:194,803,1127,1204,1474,1592`）、`merge_prod_bin.py:8,117` 打印串（0x0801C000 地址与 C 常量一致）。**无 META_VERSION、无 28B/272B、无 crc32@0x18 偏移常量** → 不存在 v2/v3 布局漂移风险面。
2. **诊断消费走帧语义而非结构字节**：`zcanpro_boot_diag_capture.py` 解析 Boot 诊断帧 M1 `0xA1`：固件 `boot_safe_mode.c:144-152` 发 `p[0]=0xA1 p[1]=app_valid p[2]=meta_src p[3]=magic_ok p[4]=(version==META_VERSION) p[5]=crc_ok(sizeof-4)`；宿主解析 :157-159 `{app_valid:b[1], meta_src:b[2], magic_ok:b[3], ver_ok:b[4], crc_ok:b[5]}` —— **逐字节对应 [MATCH]**；META_SRC 语义表 :65-69 覆盖固件全部取值 `0=主区 1=备区 2=defaults 0xFF=未记录`（`boot_metadata.c:173,183,191` + `boot_safe_mode.c:138-140` 注释）**[MATCH]**。CRC 判定语义（sizeof(ota_metadata_t)-4=24=0x18）与 v3 定义一致。
3. **双工程 C 同源性复核**：`ota_trigger.h:87-101` vs `boot_metadata.h:76-89` 字段名/顺序/尺寸逐项相同（28B）；两侧 `META_CRC32_OFFSET=(sizeof(ota_metadata_t)-sizeof(uint32_t))` 同式（`ota_trigger.c:20`、`boot_metadata.c:19`）→ 恒等 0x18，不随结构演算漂移；`version!=3 拒绝`双侧同逻辑（`ota_trigger.c:31`、`boot_metadata.c:105`）→ v2(272B) 数据必然被拒重建，与 d3b550d 声明一致 **[MATCH]**。默认值唯一差异 `last_boot_reason`：App 侧 0（`ota_trigger.c:52`）vs Boot 侧 `BOOT_REASON_POWER_ON=0x00`（`boot_metadata.c:127`）——**同值**，非漂移。

---

## D. 旧值残留扫描（全仓 Python，排除 .git/__pycache__）

| 扫描项 | 结果 | 结论 |
|---|---|---|
| 镜像头 version 字段读写残留 | 固件 grep `hdr->version/header->version/image_version/IMAGE_VERSION`：仅命中 `device_info.c:70`（DeviceInfo 自有 version，语义正确）与 `ota_trigger.c:31`/`boot_metadata.c:105`/`boot_safe_mode.c:150`（**metadata.version==3 校验**，语义正确）。**无任何代码读写镜像头 @0x4C** | **[EXCLUDED]**（无残留） |
| Python version 相关代码 | 全部为 `hdr_reserved_ver`（:190-191,:704-705；verify_image.py:35,146,162）与 docstring 说明（pack_image.py:10） | **[MATCH]**（新语义，非残留） |
| slotB/slot_a/slotA 旧架构残留 | `zcanpro_ext_ota_auto.py:202-205` `SLOT_A/SLOT_B/SLOT_A_BASE/SLOT_B_BASE/SLOT_SIZE` —— 注释明示 "deprecated aliases kept so legacy helpers still compile"；:203-204 两别名**全仓零引用**；`SLOT_SIZE`（=APP_SIZE）:653,698-699 有引用且语义正确（镜像上限/校验窗=App 区）；`slot_base()` :613-615 恒返 APP_BASE（docstring 标注 deprecated compat）；日志"回旧槽"字样 :663 为 fail 提示文案 | **[RISK-R5]**（死别名，功能无害）/ 其余 **[EXCLUDED]**（有意兼容层） |
| 版本号编码 `1.1.1 / 1_1_1 / 1.0.0 / 1_0_0 / 1.1.2` | `zcanpro_ext_ota_auto.py:51` `EXPECTED_SW_VERSION="QC_JYF_FW_1.1.2"`（用户 2026-09-19 指定的 OTA 目标版本，判定闭环用）、:56 `BASELINE_SW_VERSION="QC_JYF_FW_1.1.1"`（升级前基线打印）；`zcanpro_read_app_version.py:45-46,457` 期望 `QC_JYF_FW_1.1.1/QC_JYF_BL_1.0.0/QC_JYF_HW_1.1.5`；`zcanpro_boot_diag_capture.py:746-749,794` 自测向量同值 | **[EXCLUDED]**：这些版本串是 **DID 0xF195 运行版本期望值**，与固件 `can_protocol.c:53-55` 编译常量（FW_1.1.1/BL_1.0.0/HW_1.1.5）**逐一相等** —— 正是 dad004a 之后"版本号唯一真相源"的正确落点，**不是包头旧字段残留**。EXPECTED=1.1.2 是升级目标（用户侧 Keil 改 SW_VERSION_STR 后构建载荷），脚本注释 :42-50 明确"仓库固件保持 1.1.1 零触碰" |
| v2 metadata 残留（固件） | grep `272/active_slot/pending_slot/slot_valid`：仅命中 sha256.c 常量表（无关） | **[EXCLUDED]**（无残留） |
| `sign_seed.py` ecdsa 依赖 | `from ecdsa import SigningKey, der`（:23）本机无该包 → ImportError。但该脚本是 **0x27 SA 手工签名工具，不在打包 import 链**（pack_image.py 只 import zcanpro_ext_ota_auto，纯 Python ECDSA），且 `脚本使用说明.md:9` 已文档化 "sign_seed.py 额外需要 pip install ecdsa" | **[EXCLUDED]**（已文档化的独立工具依赖） |

---

## E. 执行验证记录（读代码≠验证代码）

| # | 执行项 | 结果 |
|---|---|---|
| E1 | `pack_image.py --help` | rc=0，import 链完整（`from zcanpro_ext_ota_auto import APP_BASE/IMAGE_HEADER_SIZE/load_ec_private_key/pack_image_if_needed/validate_image` 全部解析成功）**[MATCH]** |
| E2 | `verify_image.py --help` | rc=0 **[MATCH]** |
| E3 | `merge_prod_bin.py --help` | rc=0 **[MATCH]** |
| E4 | `zcanpro_ext_ota_auto` 模块导入 + 常量运行时读取 | import OK（`import zcanpro` 为 try/except 可选依赖，:29-32）；实测 `IMAGE_MAGIC=0x4F544158 IMAGE_HEADER_SIZE=256 APP_BASE=0x08004000 APP_ENTRY=0x08004100 HDR_VER_OFF=0x4C HDR_VER_LEN=16 HDR_TS_OFF=0x5C APP_SIZE=0xC000` **全部 [MATCH]** |
| E5 | `sign_seed.py --help` | `ModuleNotFoundError: No module named 'ecdsa'` → **[EXCLUDED]**（§D 表末行） |
| E6 | **合成 roundtrip**：crafted payload（SP=0x20005000, Reset=0x08004109, 108B）→ `pack_image_if_needed`（真实 `docs/keys/private.pem`）→ 字节 dump | @0x00=0x4F544158 ✓ @0x04=108 ✓ @0x08=zlib.crc32 ✓ @0x0C..0x4B=64B ✓ **@0x4C..0x5B=16×0x00** ✓ @0x5C=unix ts ✓ @0x60..0xFF=160×0x00 ✓ 头=256B ✓；脚本内 `_selfcheck_image` 纯 Python 仿射验签 **PASS**（public.pem 独立验签通过） |
| E7 | `verify_image.py /tmp/d4_test_image.bin`（无 --key） | magic/length/crc32/reset-window 全 OK，`hdr @0x4C = 0000...0000`，**RESULT: PASS rc=0** **[MATCH]** |
| E7b | `verify_image.py --key docs/keys/public.pem` 同一镜像 | **RESULT: FAIL（ecdsa: openssl exit 1）→ [MISMATCH-M1]** |
| E7c | **openssl 双向对照实验**（定责）：同一签名/同一密钥/同一 payload | ①正确用法 `openssl dgst -sha256 -verify docs/keys/public.pem -signature sig.der payload.bin` → **"Verified OK" rc=0**（证明镜像、签名、密钥全部有效，签名域=payload 单次 SHA256）；②复现 verify_image.py 缺陷 a：其写出的 pub 文件仅 **64B**（`pem_to_sec1` 返回无 04 前缀的 X\|\|Y（:87 返回 `der[j+2:j+66]`），:114 `b"\x04"+sec1[1:]` 又丢 X 首字节；且 openssl `dgst -verify` 需 PEM/DER 公钥文件，裸 SEC1 不可读）→ `Could not read public key rc=1`；③复现缺陷 b：:115 把 `SHA256(firmware)` 写入文件后 :117 用 `openssl dgst -sha256` **再哈希一次**（双重哈希≠签名域）→ `Verification failure rc=1` |
| E8 | `merge_prod_bin.py` 正向（真实 `bootloader.bin`(11576B) + 测试镜像 → /tmp） | rc=0；合并产物 16748B=0x4000+364；`merged[0x4000:0x4004]=b'XATO'` ✓；boot 区尾部 0xFF 填充 ✓；布局打印 Boot 0x08000000 / App 0x08004000 / Backup 0x08010000 / Metadata 0x0801C000 **[MATCH]** |
| E9 | `merge_prod_bin.py` 负向（裸 bin 无头） | 打印 `WARNING: APP image does not start with XATO magic` 但**照常合并 rc=0** → **[RISK-R2]**（warn-only 闸门） |
| E10 | `pack_image.py` 真实默认路径运行 | `skip (not found): .../mdk_project/Objects/qi_wireless_code_app.bin` rc=1 —— 仓库无 Keil 构建产物（`app bin/` 目录亦空），**[EXCLUDED]**：布局一致性已由 E6 合成 roundtrip 执行级证明（与载荷内容无关）；真实 bin 在手时可直接复跑 E7 流程 |

---

## F. 发现清单

### MISMATCH（1 项）

**M1. `verify_image.py --key` ECDSA 辅助函数实现缺陷 → 合法镜像假阴性 FAIL**
- 位置：`python_tools/1.packaging script/verify_image.py:102-123`（`verify_ecdsa`），具体：
  - **:114** pub 文件构造错误：`pem_to_sec1`（:70-88）返回的是 **X||Y 64B（无 04 前缀）**，`b"\x04"+sec1[1:]` 丢弃 X 首字节得 64B 畸形点；且 openssl `dgst -verify` 需要 **PEM/DER 格式公钥文件**，裸 SEC1 字节必然 `Could not read public key`（E7c② 实证）。
  - **:109,:115,:117** 双重哈希：`f_dgst.write(hashlib.sha256(firmware).digest())` 后 `openssl dgst -sha256 ... dgst_path` 对该摘要**再做一次 SHA256**，与签名域（对 payload 单次 SHA256，固件 `boot_verify.c`/`ota_download.c` 与打包 `_selfcheck_image` 三方一致）不符 → `Verification failure`（E7c③ 实证）。
- 影响：装有 openssl 的任何主机上，`verify_image.py --key docs/keys/public.pem` 对**完全合法**的镜像必报 FAIL rc=1（E7b 实测）；docstring 用法示例（:10-11）恰恰引导用户带 --key 运行。**不影响**：打包字节布局、`pack_image_if_needed` 内 `_selfcheck_image`（纯 Python 仿射验签，E6 PASS）、固件 Boot/App 验签路径、`verify_image.py` 无 --key 时的结构校验（E7 PASS）。
- 定责证据：E7c① 同签名+同密钥+同 payload 用正确 openssl 用法 `Verified OK rc=0` —— 镜像/签名/密钥三方均有效，缺陷 100% 在 verify_ecdsa 实现。
- **修复建议**（改 `verify_image.py` 两行）：
  1. :114 → 改为直接写入 PEM 公钥原文：`f_pub.write(open(public_key_path, "rb").read())`（删除/绕过 `pem_to_sec1` 的裸点输出；`p1363_to_der` 的 R||S→DER 转换 :55-67 经 E7c① 证实正确，保留）。
  2. :115 → 改为 `f_dgst.write(firmware)`（把 payload 原文交给 openssl 做**单次** SHA256，与签名域对齐）。
  - 修复后重测：`verify_image.py <image> --key docs/keys/public.pem` 对 E6 镜像应输出 `ecdsa: OK openssl exit 0`、`RESULT: PASS`。

### RISK（非阻断）

| # | 位置 | 描述 | 建议 |
|---|---|---|---|
| R1 | `verify_image.py:146,162`；`boot_verify.c`（无 0x4C 检查）；`ota_download.c`（同）；`zcanpro_ext_ota_auto.py` `_selfcheck_image`（:638-686 无 0x4C 检查） | **@0x4C 零填充无任何层强制校验**：固件不读该区、verify 脚本只打印不过闸、打包自检不查。"version 区固定填 0x00" 是约定而非被验证属性；旧 version 字节残留的镜像仍可通过全部校验（功能无害——该区无消费者，但字节级承诺不可举证） | 可选：`verify_image.py` 对 `reserved_ver != 00` 时打 WARNING（不建议硬闸，避免误伤历史镜像） |
| R2 | `merge_prod_bin.py:98-102` | XATO magic 检查为 **WARNING 非硬闸**（E9 实证：无头裸 bin 照常合并 rc=0）→ 产线误用未打包 bin 会产出烧录后 Boot 直接 safe-mode 的 prod bin | 建议：缺 XATO 头默认 hard-fail，提供 `--force` 逃生口 |
| R3 | 全链路（设计既定） | 头部除 magic/length/crc32 外字段（@0x4C 保留区、@0x5C build_timestamp、@0x60 reserved）**不在 ECDSA 签名域内**——签名只覆盖 payload，各层（Boot `boot_verify.c:167-182`、App `ota_download.c:238-250`、Python 打包/自检/校验）行为一致；该性质在包头优化提交前后未变化 | 信息性，无需动作；若未来 build_timestamp 需防篡改，需扩签名域（属架构变更） |
| R4 | `qi_wireless_bootloader/mdk_app/Src/boot_verify.c:91` | magic 使用字面量 `0x4F544158U`，boot 工程无 `IMAGE_MAGIC` 共享宏（App 侧有 `OTA_IMAGE_MAGIC`）——值本身 MATCH，属可维护性风险（改 magic 需改多处字面量） | 可选：boot 工程补 `#define IMAGE_MAGIC 0x4F544158U` 并替换字面量 |
| R5 | `python_tools/zcanpro_ext_ota_auto.py:203-204` | `SLOT_A_BASE/SLOT_B_BASE` 废弃别名全仓零引用（死代码）；注释已声明 intentional compat | 可选：下轮脚本清理时删除（`SLOT_SIZE` 有引用须保留） |

### EXCLUDED

- `sign_seed.py` 缺 `ecdsa` 包（E5）：独立 0x27 手工签名工具，不在打包 import 链，`脚本使用说明.md:9` 已声明依赖。
- 仓库无 Keil 构建产物（E10）：`app bin/` 与 `mdk_project/Objects/*.bin`（App）为空/缺失 → 无法对真实镜像跑 verify；布局一致性已由合成 roundtrip 执行级证明（E6/E7）。
- 版本串 `QC_JYF_FW_1.1.1/1.1.2` 等：DID 0xF195 运行版本期望值，与 `can_protocol.c:53-55` 编译常量一致，是 dad004a 后版本号唯一真相源的正确落点，非包头旧字段残留（§D）。
- `SLOT_A/SLOT_B/slot_base()` 等兼容层与日志旧措辞：注释明示 deprecated compat，语义已全部收敛到单 App 架构（slot_base 恒返 APP_BASE）。

---

## G. 结论

1. **打包脚本与代码"包头数据优化"完全匹配**：`zcanpro_ext_ota_auto.py:pack_image_if_needed` 构造的 256B 头与 `ota_trigger.h ota_image_header_t`、`boot_verify.h ota_image_view_t` 逐字段逐偏移一致（执行级字节证明 E6）；@0x4C 16×0x00、@0x5C 时间戳、@0x60 160B 补齐、签名域=payload-only，五层（App C / Boot C / pack / verify 结构 / merge）全一致。
2. **常量矩阵零漂移**（§B）：256 / 0x4F544158 / 0x08004000 / 0x08004100 / 0xC000 / 0x08010000 / 0x08000000 / 0x4000 / META 0x4F54414D v3 / 0x0801C000 / 0x0801C800 在 C 双工程与 Python 各脚本间全部同值。
3. **metadata v3(28B) 对打包脚本零耦合**：Python 侧不构造/解析 metadata 字节；诊断脚本走帧语义且与固件逐字节对应；双工程 C 同源 + `META_CRC32_OFFSET=sizeof-4` 同式 + `version!=3 拒绝`双侧同逻辑。
4. **旧值零残留**：无任何代码读写镜像头 version 字段；Python 中版本串均为固件 SW_VERSION_STR 同源的运行版本期望值（正确落点）；slot 别名为已声明的死兼容层。
5. **唯一 MISMATCH（M1）**：`verify_image.py` 的 `--key` openssl 验签辅助函数实现缺陷（公钥文件格式 + 双重哈希两个独立根因），导致合法镜像假阴性 FAIL。打包链路与固件验签路径不受影响。**建议按 §F-M1 两行修复后单独重测 `verify_image.py --key`**；打包/合并/固件侧无需改动即可放行。

- verdict: **PASS_WITH_RISKS**
- confidence: **92**（执行级验证覆盖 pack→verify→merge 全链路与 openssl 双向定责实验；扣分项：真实 Keil bin 缺失以合成载荷替代、sign_seed.py 环境未补齐——二者均不影响本审计结论的布局一致性判定）

审计人：evaluator subagent（D4）｜只读审计，零改码；测试产物：/tmp/d4_*，本报告为唯一落盘文件。
