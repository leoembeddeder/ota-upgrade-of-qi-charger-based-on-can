# OTA-ARCH-0920-R3 python_tools 脚本 + 脚本使用说明再审核报告（evaluator）

- 基线：origin/main=408e9a3（WSL 仓）；审核范围=python_tools 全部 17 脚本+脚本使用说明.md；只读零改码
- **verdict: PASS_WITH_RISKS**（blocking=0；should_fix=6，info=4；主管回 coder 修后本席复验）

## 第一节：I 盘核查记录（主管核实，如实记录）

- I 盘生产仓 HEAD=408e9a3 与 WSL 同步 ✓；盘面文件与 WSL 的 diff=**CRLF 换行转换**，非内容分歧（I 盘 git status 仅 2 个 uvprojx 本地修改、脚本零修改佐证）——本报告结论对 I 盘同样有效。
- 提醒：I 盘 2 个 uvprojx 本地未提交修改（构建设置关联）——建议用户提交或还原，避免与仓库状态混淆（info）。

## 第二节：逻辑正确性+常量三方对照（维度1/2）— PASS

- **常量三方对照全同**：zcanpro_ext_ota_auto.py APP_BASE=0x08004000(:195)/APP_SIZE=0xC000(:196)/BACKUP_BASE=0x08010000(:198)/BACKUP_SIZE=0xC000(:199)/DOWNLOAD_ADDR=0x08010000(:174 "0x34 target=Backup")/IMAGE_MAGIC=0x4F544158(:185)/IMAGE_HEADER_SIZE=256(:186)/EXPECTED=1.1.2(:51)/BASELINE=1.1.1(:56)；verify_image.py :23-29 同值；merge_prod_bin.py BOOT_BASE=0x08000000(:35)/BOOT_SIZE=0x4000(:36)/APP_BASE 推导=0x08004000(:39)——对照 C 端 boot_metadata.h:39-44（APP/BACKUP/HEADER/ENTRY）+ota_trigger.h:35-49（OTA_ 同值+OTA_IMAGE_MAGIC+OTA_META_VERSION=3U）**逐项一致** ✓。
- **流程对照成立**：脚本 10 02→27→31 擦备份区→0x34@0x08010000→0x36→0x37→判定闭环 vs ota_download.c:312(g_base=OTA_BACKUP_BASE_ADDR)/:317(活跃区防护 g_base==OTA_APP_BASE→NRC)/:273(commit backup_valid=1)/:617(0x37 后 NVIC_SystemReset) + Boot 搬运（boot_trial.c）+0xF195==EXPECTED 判定——逐步对应 ✓。
- **边界路径在位**：选 bin fail-closed（ota_auto :141/:154-155 "无任何候选 bin 版本==EXPECTED…旧版本残留绝不被选中"）；拒闪门 fail-closed（:46-49 注释+门代码）；XATO 头校验 raise（:620-626 镜像太短/缺头/length 不符）；capture 零帧二分+raw_dead_hint+降级兜底；pack 自检 App 窗口 [0x08004100,0x08010000)（verify_image.py docstring+:93 verify_reset_handler）✓。
- **独立复测**：py_compile 17/17 全 OK；capture 自测 21/21 PASS（@408e9a3 blob 宿主机实跑）✓。

## 第三节：findings（file:line / issue / fix）

| # | level | file:line | issue | fix |
|---|---|---|---|---|
| 1 | should_fix | 脚本使用说明.md:25 | "Metadata 0x0801C000/0x0801C800，272B×2 双副本，META_VERSION=2"——与 v3 代码现实（28B/crc32@0x18/META_VERSION=3，d3b550d+Q2F 已定稿）不符 | 改为"28B×2 双副本，META_VERSION=3（v2 及以前拒收→defaults 重建）" |
| 2 | should_fix | 脚本使用说明.md:62-64 | DID 判读口径缺 0x2115（0x2113 恒 0x00/0x2114=0x02\|0xFE/0x2116 已列，0x2115=last_boot_reason 缺失） | 补 0x2115 行（last_boot_reason：00=POR/01=SW/02=WDG/03=OTA/04=copy失败） |
| 3 | should_fix | 脚本使用说明.md:54-67（OTA 节） | 缺 v2→v3 metadata defaults 重建说明+搬运断电恢复口径（维度3点名项） | 补两段：设备旧 v2 metadata 升级后被重建属预期（历史字段归零）；backup_valid 未清断电→BOOT 下轮上电整链重试（幂等） |
| 4 | should_fix | 脚本使用说明.md:56-67 | 版本口径未反映现状：EXPECTED=1.1.2 与仓库 can_protocol.c:53 SW_VERSION_STR=1.1.1（用户尚未对齐）的关系未写明 | 补"当前仓库固件 1.1.1；升级前须改 SW_VERSION_STR=QC_JYF_FW_1.1.2 后 Rebuild，否则拒闪门拦截" |
| 5 | should_fix | 2.functional test script/ 六脚本：charge_start:543-545/:570、charge_stop:726-728/:753、qi_set_power:439-449/:458、sa_lockout_test:760-779、sn_read:193-212、sn_write:498-517 | A/B 时代注释/FAIL_STEP 表残留："slot=ota_running_slot()=0x00/0x01"（现 shim 恒 0x00）、"select_boot_slot 无有效槽（metadata 无 active/trial 槽）"（该代码路径已删）、can_protocol.c/ota_trigger.c 行号引用漂移——**行为兼容**（探测判定"slot 字节≠0xFE=APP"仍成立：APP=0x00、safe-mode 应答 05 62 21 13 FE 不变），但叙述误导现场排障 | 注释/表项按单 App 语义更新：0x2113 恒 0x00 仅信息展示；FAIL_STEP 表改引 verify fail_step/copy_fail_step 新语义 |
| 6 | should_fix | zcanpro_qi_iap_log1.py:197 + zcanpro_qi_iap_log2.py:197 | pyflakes：**undefined name 'binascii'**（未 import）——该代码路径触发必 NameError（pre-existing 缺陷，非 OTA 架构批引入；Qi 芯片 IAP 转发链脚本） | 补 import binascii 或改用已导入模块 |
| 7 | info | sign_seed.py:22/:94 | pyflakes：'struct'/'ecdsa.NIST256p' imported but unused（coder Q2 已披露 pre-existing） | 清理 import |
| 8 | info | iap_log1/log2 :22/:301/:495 | struct unused + global stopTask 未赋值（风格项） | 随 #6 一并清理 |
| 9 | info | zcanpro_read_app_version.py:37/:442 | 期望 SW=QC_JYF_FW_1.1.1 与现状 can_protocol.c:53 一致 ✓（正确）；用户版本对齐 1.1.2 后此脚本期望值需同步 | 版本切换时更新期望值 |
| 10 | info | I 盘（主管核实） | 盘面 vs WSL diff=CRLF 换行（非内容分歧）；I 盘 2 个 uvprojx 本地未提交修改 | 提醒用户提交或还原本地 uvprojx 修改 |

## 第四节：doc_mismatch 汇总（脚本使用说明.md vs 脚本实际）

1. :25 metadata 行 v2 残留（272B/META_VERSION=2）→ 应为 28B/v3（finding #1）
2. :62-64 DID 判读表缺 0x2115（finding #2）
3. 缺 v2→v3 重建+断电恢复说明（finding #3）
4. 缺版本现状口径（EXPECTED 1.1.2 vs 固件 1.1.1 未对齐）（finding #4）
- 核对通过项：:3-5 架构描述、:22-24/:26 分区常量与 XATO 头行、:28-52 pack/verify/merge 用法（默认路径=qi_wireless_code_app Objects/qi_wireless_code_app.bin 与 pack_image.py:37-38 DEFAULT_BIN 一致 ✓）、:56-67 OTA 流程与判定闭环主口径、:69-81 诊断帧 M1~M4/通信层 v2/零帧二分、:83-87 常量维护节。

## 第五节：功能测试 9 脚本 + Qi IAP 脚本（维度4）

- **行为兼容（新架构下功能有效）**：charge_start/charge_stop/qi_set_power/sa_lockout_test/sn_read/sn_write——设备探测链=22 2113 slot 字节≠0xFE→APP 判定：新架构 APP 恒答 0x00（shim ota_running_slot()=0U）≠0xFE ✓；safe-mode 检测=0xFE 应答（boot_safe_mode.c 探测应答格式 05 62 21 13 FE <step> 未变）✓；UDS_REQ/RESP=0x18DA0D03/0x18DA030D 未变 ✓。陈旧点仅为注释/表项叙述（finding #5）。
- **qi_read_status/qi_read_version**：Qi 芯片面（状态/版本读取），无 AT32 DID/架构依赖 ✓。
- **read_app_version.py**：只读 DIDs 0xF195/0xF180/0xF193；期望值与现状一致（finding #9）。
- **iap_log1/log2**：Qi 芯片 IAP 固件推送（UDS→MCU→UART 转发），与 AT32 Flash 分区/DID 语义无关（docs/4 定性沿用）✓；发现 pre-existing binascii 缺陷（finding #6）。

## 第六节：边界与异常路径（维度5）

- 无应答：UDS 探测重试+NRC 定性报错（_erase_with_retry :961-972 NRC 0x72 形态区分）；capture 探活 3E/降级兜底/raw_dead_hint 判据链 ✓。
- 版本不符：选 bin fail-closed（旧版本绝不被选中）+拒闪门 raise（零业务流量）双保险 ✓（:141/:154-155）。
- CRC/验签错：C 端 verify fail_step 1~6+NRC；脚本判定闭环差异明细+判别矩阵指引 ✓。
- 搬运中断电：脚本侧判定闭环自动补发 11 01+断电重试口径（:808-829）；文档侧说明缺失（finding #3）。

**evidence_digest**：①常量三方对照逐项全同（脚本 vs boot_metadata.h:39-44/ota_trigger.h:35-49）+流程对照（ota_download.c:312/:317/:273/:617）成立；②py_compile 17/17+capture 自测 21/21+边界 fail-closed 路径 grep 在位；③使用说明 4 处过时（:25 v2 残留/缺 0x2115/缺 v2→v3+断电说明/缺版本现状）；④功能测试 9 脚本行为兼容（0xFE 判定链+UDS ID 未变），6 脚本注释表项 A/B 残留；⑤iap_log1/2 :197 binascii 未定义=真实运行期缺陷（pre-existing）。

**confidence=85**（逻辑/常量/边界维度证据充分；扣分=功能脚本内部业务逻辑未逐行复核（抽样 DID/CAN-ID/探测层）+I 盘实盘采信主管核实未亲验）。

*—— 评估员已完成 R3 再审核 | 只读零改码；findings 修复后本席复验*
