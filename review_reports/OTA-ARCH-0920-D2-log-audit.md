# OTA-ARCH-0920-D2 实测总线静默审计报告（只读，零改码）

基线：git pull 后 HEAD=f3cd196（代码层 3dc0649，其后为 docs 提交 adae100/b66b0b1/f3cd196）
症状：22:28 zcanpro_read_app_version.py——UDS 3E 00×2 无应答（timeout）；deinit 后 raw 嗅探 1.5s 共 0 帧（receive 返回 (1,[])）；脚本 Tx=0x18DA0D03/Rx=0x18DA030D；用户"Keil 没有直接烧录"（烧录工具/内容未声明，据 docs f3cd196 FAQ 上下文为合并 bin 路径）。
关键对照：同机同通道同脚本 10:33 实测全通（3E 00→7E 00 + 22 F195/F180/F193 三 DID 读出）→ 脚本引擎与库路径已被实机验证，22:28 失败的变量在设备侧/总线侧。

## 一、候选根因（按嫌疑度排序）

### 【高】设备未运行可应答固件 / CAN 物理层 TX 不通
- 机理：UDS 3E 00 两发全 timeout=设备无任何 UDS 应答；若 MCU 未执行到 CAN 上线（烧录失败/内容错误/Boot 卡死）或 CAN TX 物理不通（接线/终端电阻/TXD 极性→bus-off），测试端呈现完全静默。
- 证据：
  - 10:33 同机全通（同脚本同库同通道）→ 脚本侧排除；
  - lifecycle.c:100（qi_wireless_code_app）："TXD 极性错误时会连着 bus-off，BOOTUP 会刷屏且 UDS 全超时"——历史同签名失败形态；
  - 用户烧录方式/内容未声明；烧录工具与实际写入内容（哪份 bin、哪个基址）无闭环证据。
- 判定：**当前最强候选**；代码审计未发现必然导致静默的逻辑缺陷（见排除项），剩余解释=烧录内容/流程问题或硬件层。

### 【中】"0 帧"raw 嗅探=库语义假象，不构成总线静默证据
- 机理：zcanpro 库 receive() 仅在 uds_init 调用过之后才返回真实帧；uds_deinit 之后 receive 是否回真实帧**库行为未验证**。
- 证据：zcanpro_read_app_version.py:368 注释（实测语录）："V1.0.0：必须先 uds_init，ZLG 才会开接收。**不 init 时 receive 恒为 (1,[])**"——只证明"未 init→空"，未证明"deinit 后→空"是否同样成立；脚本 :396-398 序列=UDS 失败后 `_uds_deinit()`→can_recv 嗅探——若 deinit 关闭了接收路径，(1,[]) 是结构性结果而非总线证据。
- 判定：**22:28 "0 帧"证据效力=弱**；唯一硬证据=UDS 3E 00 超时（走已验证的 uds_request 路径）。后续判读不得以"raw 0 帧"单独定罪总线静默。

### 【中】设备新固件首启 CAN 未上线（唤醒路径待实证）
- 机理：SIT1145 收发器上电默认 Standby（can_protocol.c:135 注释"Power-on default is Standby"；main.c:58"SIT1145 is in Standby. BOOTUP/OPERATIONAL after first CAN wake"）——即使 CAN_LP_STANDBY_ENABLE=0（can_protocol.h:63，仅关"空闲 180s 进休眠"），上电默认 Standby 的首次唤醒链仍是必经路径；若新固件该路径存在实机回归（本次代码审计未发现门控缺陷，但未经实机验证），设备 CAN 永不上线=静默。
- 证据：can_protocol.h:63 ENABLE=0；can_protocol.c:135/:150（CAN_LP_WAKE_INHIBIT_MS=100U 唤醒抑制窗）/:158（180s idle 超时常量）；lifecycle.c:126"BOOTUP is sent on first SIT1145 wake (can_protocol_poll), not at power-on"。
- 判定：代码层无缺陷证据，但新固件未经实机 CAN 验证，保留为待帧证据判别的开放项。

### 【低·排除】CAN ID 不一致
- 脚本 zcanpro_read_app_version.py:26-27 `0x18DA0D03/0x18DA030D` vs 固件 can_driver.h:42-43 `CAN_ID_UDS_REQUEST/RESPONSE=0x18DA0D03U/0x18DA030DU` vs can_protocol.h:42-43 `CAN_PROTO_UDS_REQUEST/RESPONSE` 同值——**三处逐字节一致**。
- 过滤器：can_driver.c:102-110 Filter0 code=CAN_ID_UDS_REQUEST、mask=0x1FFFFFFF（精确匹配 29 位扩展帧）→脚本 Tx ID 落在监听窗内；:114-120 Filter1=0x18DB3300/mask 0x1FFFFF00（functional）。
- 今日改动波及面：git diff 6e9ab04^..3dc0649 对 can_protocol.c/can_driver 的 ID 相关行仅为 rename 伴随的整文件出现（slotA→code_app 路径变更），ID 层内容零修改。
- 判定：排除；请求 ID 会被 MCU 过滤器接收（前提=固件在跑）。

### 【低·排除】Boot 首启 app_valid 门控缺陷（任务书候选根因，代码层证伪）
- main.c 跳转决策链（qi_wireless_bootloader/mdk_user/Src/main.c）：:35 boot_metadata_init→:39 boot_backup_pending？→copy 尝试（失败不拦截，:50 注释"try current App image anyway"）→**:60 `if (boot_app_image_ok() == 0)`→:62 boot_jump_to_app(APP_ENTRY_ADDR)**→:66 失败才 enter_safe_mode。
- **跳转门控=boot_app_image_ok()（直读 0x08004000 XATO 头验 magic/len/CRC/ECDSA/向量），与 meta.app_valid 无关**；app_valid 全链无读消费：仅 boot_trial.c:182（copy 成功后写 1）与 boot_metadata.c:124（defaults 写 0）两处写点。
- 合并 bin 首启场景：metadata 全 FF→meta_validate 失败（v3 校验拒绝）→defaults 重建（app_valid=0 但不影响）→backup_valid=0→跳过 copy→直验 App 区→镜像合法即跳转。
- 判定：**"app_valid=0 门控不跳"候选根因证伪**，无需修复清单项。

### 【低·排除】APP 侧 CAN 启动被 metadata/trial 残留阻塞
- main.c（code_app）:51-59 初始化链：board_gpio→nvm_drv→**can_driver_init(:53)→can_protocol_init(:54)**→qi_uart→__enable_irq(:56)→lifecycle_init(:59)——**无条件执行，无 metadata 读取门控**。
- can_protocol_init 本体（can_protocol.c:1912-1924）：会话/安全标志复位+isotp_init+RX 回调注册+qi_protocol 回调——**零 metadata 依赖**；can_driver.c 全文 grep metadata/ota_state/backup_valid 门控=0 命中。
- ota_trial 接线删除残留核查：main.c 主循环 poll 列表=timer/can_protocol/can_driver/qi_uart/lifecycle/board_charge——**无 ota_trial 残留**，无卡死路径。
- 判定：排除；v2→v3 metadata 重建不会阻塞/延迟 CAN 上线。

### 【低·排除为主】merge_prod_bin 布局数学
- merge_prod_bin.py:36-39：BOOT_SIZE=0x4000、APP_OFFSET=BOOT_SIZE、APP_BASE=BOOT_BASE+0x4000=0x08004000；合并=boot(16KB 补 0xFF)+app XATO 镜像→头@0x08004000、代码@0x08004100——**偏移数学闭合**，与 Boot 验签窗 [0x08004000,0x08010000) 一致。
- 默认路径衔接：pack_image.py:37/:40 DEFAULT_BIN=q…code_app/.../Objects/qi_wireless_code_app.bin、DEFAULT_OUT_NAME="app_image.bin"↔merge_prod_bin.py:32 DEFAULT_APP="app bin/app_image.bin"——**pack 默认输出=merge 默认输入，链路闭合**；uvprojx:51 OutputName=qi_wireless_code_app 与 pack 默认输入名一致。
- 残余风险（非布局数学问题）：**用户实际烧录所用 bin 的生成路径/时间未声明**——若用了旧路径旧产物（如 app_slot_a.bin 时代或未 Rebuild 的陈旧 Objects 产物），烧入内容陈旧属烧录流程问题；merge :98 对非 XATO 镜像仅 WARNING 不拦截。
- 判定：布局与脚本衔接排除；待用户声明烧录物来源后可进一步闭环。

## 二、修复清单（发现必修缺陷=无；以下为待派单建议，本批不执行）

1. 【现场判别实验·零改码】烧录已就绪的诊断版 Boot（60d1137 构建 16208B，sha256=f535570e…）或当前 main 合并 bin 后跑 zcanpro_boot_diag_capture.py（通信层 v2，be2df31/00d54dd/c64aca3）：M1~M4 帧+全帧统计+零帧二分直接判别"Boot 未跑/APP 未跑/CAN 物理层死"三选一。
2. 【硬件三查】CAN_H/CAN_L 接线与终端电阻；烧录器对合并 bin 的实际烧录日志（起始地址 0x08000000+内容 sha 与 merge 输出比对）；上电电流/NRST 脚电平。
3. 【可观测性增强建议（仅建议）】脚本侧：TX 每帧尝试形态与结果日志（diag v2 can_send 已有 _tx_mode 记录；建议 read 版工具补 UDS 失败时打印 zcanpro API 目录+uds_init 配置回显）；固件侧：M1~M4 诊断帧已是 Boot 链观测通道（0x18FF480D）；总线侧：CAN 分析仪看 ACK 位/示波器量 SIT1145 INH 与 CANH/L；设备应答后可读 DID 0x2119（SIT1145 LP 状态：wup_cnt/last_standby_sec，can_protocol.h:152）交叉验证唤醒史。
4. 【条件修复项】若诊断帧实测显示 Boot 链正常（M1~M4 齐）但 APP 无应答：定向审计新 APP CAN 唤醒路径实机行为（can_protocol_poll 上电默认 Standby 唤醒序列+BOOTUP 发送时机），按帧证据定位后再派修复单。
5. 【流程项】要求用户后续烧录时记录：bin 文件名/sha256/烧录工具/起始地址，避免"烧了什么"不可考。

## 三、审计结论

脚本与代码逻辑层：CAN ID 一致性、Boot 跳转门控、APP CAN 启动依赖、merge 布局数学四项**排除**（file:line 实证）；"raw 0 帧"判定为库语义弱证据（deinit 后 receive 行为未验证），UDS 超时为唯一硬证据；结合 10:33 同机全通对照，根因收敛为**设备侧烧录内容/流程、CAN 物理层、新固件 CAN 上线回归（待实证）**三方向，代码层无必修缺陷，修复清单以现场判别实验为先。
