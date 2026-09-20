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

## R3FE 复验章节（2026-09-20 21:40，HEAD=3dc0649，R3F=06296f9+6835603+3dc0649）

**verdict: PASS → R3/R3F 全案闭环**

1. **六项终态全达成**：①使用说明:25="28B×2 双副本，META_VERSION=3（v2 及以前拒收→defaults 重建）"，stale grep(272B/META_VERSION=2/0x10C)=0 ✓；②:70 0x2115 行在位（00=POR/01=SW/02=WDG/03=OTA/04=copy 失败）——与 boot_metadata.h:58-62 BOOT_REASON_* 逐值一致+can_protocol.c DID_LAST_BOOT_REASON 直读 meta.last_boot_reason 实证 ✓；③:73-75 v2→v3 重建段+ :78-80 断电幂等段在位（含代码行号引用）✓；④:83-86 版本口径现状段在位（SW 1.1.1 can_protocol.c:53 vs EXPECTED 1.1.2 ota_auto:51+升级前须改版本号 Rebuild+拒闪门 fail-closed）✓；⑤六功能脚本对 408e9a3 diff=纯注释+FAIL_STEP_DESC[0] 文档字符串（单 App 语义化改写，零行为变更行）✓；⑥iap_log1/2 :22 import binascii 在位+:197 binascii.unhexlify 使用点 ✓。
2. **回归链闭环**：sign_seed.py :23 `from ecdsa import SigningKey, der` 恢复且实际引用（:98 SigningKey.from_pem/:99 der.sigencode），unused struct import 正确移除；iap_log1/2 stopTask 配对完好（:96 模块级/:120-124 z_notify global+stop→True/:570-572 z_main global+复位 False）；z_notify 锚点假信号无残留（函数体=_log+stop 判定）；三 commit 净语义变更=六项本体+import 修复，无任务书外语义改动 ✓。
3. **终态独立复跑**：py_compile 17/17 OK+pyflakes 全部零告警（空输出）；read_app_version.py:37 期望 1.1.1 未动 ✓；capture 自测 21/21 PASS ✓。
4. **范围审计**：三 commit 文件=10 python_tools+1 review_reports，全授权无越权 ✓。

evidence_digest：①六项 file:line 全核实（:25/:70/:73-80/:83-86+六脚本 diff+import :22）；②0x2115 取值与 C 码逐值一致（boot_metadata.h:58-62+can_protocol.c 直读实证）；③回归链：sign_seed 在用 import 恢复+引用点在位、stopTask 配对/复位完好、无假信号残留；④py_compile+pyflakes 终态全零+自测 21/21；⑤范围干净零越权。confidence=94（剩余不确定性仅 MDK/实机，N5 前置沿用）。

*—— 评估员已完成 R3FE 复验 | 只读零改码*

## R4 第三轮独立检查章节（2026-09-20 22:17，HEAD=64c31be）

**verdict: PASS（无新发现 should_fix+；深查证据如下）**

1. **git 状态**：HEAD=64c31be（=R3FE 报告 commit，叠于代码基线 3dc0649）；python_tools 对 3dc0649 diff=空——**21:36 后零漂移** ✓；使用说明未变（R3FE 结论携带有效）。
2. **第三轮基准**：py_compile 17/17 OK+pyflakes 全零（独立复跑）✓。
3. **ota_auto 边界深查（新角度，全部正向证据）**：①设备中途复位：11 01 三试×0.5s 失败→非致命日志"0x37 后已自复位属预期，判定权在三条件闭环"（:829-830）→确认窗口 WINDOW_S+BLIND_PROBES=3+SafeModeError 独立路径（:1036-1059）；②0x37 失败：五次无应答→otx_anomaly=True→继续判定不判死（前置证据 1708-1716）；③0x36 传输中断：块循环 :1567-1574（seq 1..0xFF 回绕+每块 stopTask 检查）异常即抛出→会话中断→重跑=0x31 重擦+新 0x34 干净重来；MCU 侧状态机（ota_download.c:62-68 g_block_seq/g_write_addr+:70 ota_dl_abort）+0x37 verify（magic/length/CRC/ECDSA）拦不完整镜像→flag 不置→fail-closed 端到端 ✓；④TransferExit :1577-1582 五试×45s+SafeModeError 独立；⑤SA 回退：27 02 整包 NRC→自动 27 03 分片（:862-863）✓；⑥超时参数：response=3000ms/enhanced=120000ms（覆盖 0x31 擦槽）✓。
4. **打包四件套闭合**：pack_image（pack_one→pack_image_if_needed→_selfcheck_image 无条件自检①reset 窗②payload CRC③独立仿射 ECDSA，FAIL 拒产出 :708）↔ota_auto HDR 偏移（VER@0x4C/TS@0x5C :190-192）↔verify_image（MAGIC@0x00/LEN@0x04/CRC@0x08/SIG@0x0C :31-34）↔merge（Boot 16KB+镜像@0x4000）——签名/CRC 均"只覆盖头后 payload"（:692）与 MCU boot_verify/ota_download 同口径 ✓；sign_seed.py 独立作用域=0x27 SA seed 签名（docstring+sk.sign_digest(seed_hash) :98-99），与 ota_auto send_security_key（27 02/03+64B 校验 :841-863）配对闭合，与镜像链无交叉假设 ✓。
5. **CAN ID/DID 有效性（复认）**：UDS_REQ/RESP=0x18DA0D03/0x18DA030D（can_driver.h/can_protocol.h 未动）；DIAG_CAN_ID=0x18FF480D（boot_safe_mode.h）；0x2113/2114/2115/2116/0xF195/0xF180/0xF193 全部在现行 can_protocol.c 活跃（R3/R3FE 锤行号）；功能脚本 FAIL_STEP_DESC 已 R3F 语义化；Qi/IAP 脚本引用 Qi 芯片层+UDS 对——无已删 ID 引用 ✓。
6. **安全面（新角度）**：设备侧强制不可被脚本绕过（0x37 verify+boot ECDSA+验后置 flag，脚本无开关）✓；pack 自检无 skip 参数 ✓；三条 info 级记录（非缺陷）：⓪verify_image.py ECDSA 可选（--key，省略时打印 skipped，PASS≠签名已验——文档已标"可选"，建议使用说明强调）；ⓐEXPECTED_SW_VERSION="" 空串=拒闪门+外部锚定双关（文档化设计"留空恢复缺省"，需操作者刻意清空，非隐藏 bypass）；ⓒNRC 0x78 脚本层重试分支在 ZCANPRO 库上可能不触发（:736-738 文档化限制，120s enhanced_timeout 兜底）。

**new_vs_r3fe=无新发现（should_fix+级）**；本轮产出=三边界场景正向证据+三条 info 安全面记录，均为前两轮未覆盖角度的确认，非凑数。confidence=88（0x36 中断恢复为静态代码级论证；ZCANPRO 库行为为文档化定性）。

*—— 评估员已完成 R4 第三轮检查 | 只读零改码*
