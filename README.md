# Qi 无线充电模块 — CAN-UDS OTA 固件升级系统

> 基于 AT32F426 的车载 Qi 无线充电模块，通过 CAN 总线实现 UDS 诊断与双槽 OTA 空中固件升级。

---

## 目录

- [1. 项目概述](#1-项目概述)
- [2. 系统拓扑](#2-系统拓扑)
- [3. 核心特性](#3-核心特性)
- [4. 仓库目录结构](#4-仓库目录结构)
- [5. Flash 空间布局](#5-flash-空间布局)
- [6. 协议栈架构](#6-协议栈架构)
- [7. OTA 升级流程](#7-ota-升级流程)
- [8. Bootloader 启动流程](#8-bootloader-启动流程)
- [9. Python 工具集](#9-python-工具集)
- [10. 构建与烧录](#10-构建与烧录)
- [11. 文档索引](#11-文档索引)
- [12. 功能开发状态](#12-功能开发状态)

---

## 1. 项目概述

### 1.1 项目目标

为车载 Qi 无线充电模块开发完整的 CAN-UDS OTA 固件升级方案，满足整车级 ECU 远程刷写需求。

### 1.2 硬件平台

| 项目 | 规格 |
|------|------|
| 主控 MCU | AT32F426KBU7-4 (Cortex-M4F, QFN32) |
| 主频 | 180 MHz (HEXT 8MHz + PLL) |
| Flash / SRAM | 128 KB / 20 KB |
| CAN 收发器 | SIT1145 (SPI 配置 Normal Mode) |
| CAN 总线 | CAN 2.0B, 29-bit 扩展帧, 250 kbps |
| 无线充接口 | USART2 (PA2/PA3, 9600 8N1) ↔ Qi 芯片 |
| 霍尔传感器 | PA0 — 磁场检测 (有磁=低/无磁=高) |
| 供电控制 | PB1 (12V Buck, 低有效), PB2 (5V Qi, 高有效) |
| 调试接口 | SWD (PA13/PA14), USART1 预留 (PB6/PB7) |

### 1.3 工程组成

项目包含 **两个独立 Keil 工程** + **一套 Python 工具链**：

| 工程 | 目录 | 职责 |
|------|------|------|
| Bootloader | `qi_wireless_bootloader/` | 上电引导、镜像验签、双槽选择、Safe Mode UDS 下载、Trial Boot 管理 |
| APP Slot A | `qi_wireless_code_slotA/` | Qi 充电业务、CAN 生命周期广播、OTA 触发 (进入 Boot) |
| APP Slot B | `qi_wireless_code_slotB/` | 同 Slot A，IROM 基址不同 (0x08011900)，用于 A/B 交替升级 |
| 工具集 | `python_tools/` | 镜像打包、签名、合并、验证、一键 OTA、功能测试脚本 |

---

## 2. 系统拓扑

### 2.1 整车网络拓扑

```
┌─────────────────────────────────────────────────────────────────────┐
│                        整车 CAN 总线 (250 kbps)                     │
│                     29-bit 扩展帧, 120Ω 终端                        │
│                                                                     │
│  ┌──────────┐          ┌──────────┐          ┌──────────────┐      │
│  │   CCU    │  CAN Bus │  Qi 模块  │  USART2  │  Qi 充电芯片  │      │
│  │ (主机)   │◄────────►│ (本项目)  │◄────────►│  (无线充)    │      │
│  │ 0x03     │          │ 0x0D     │  9600bps  │              │      │
│  └────┬─────┘          └────┬─────┘          └──────────────┘      │
│       │                     │                                       │
│       │ UDS 诊断            │ 霍尔 PA0                              │
│       │ ISO-TP              │ (磁场检测)                            │
│       │                     │                                       │
│  ┌────┴─────┐          ┌────┴─────┐                                 │
│  │ PC 工具   │          │ SIT1145  │                                 │
│  │ ZCANPRO  │          │ CAN 收发器│                                 │
│  │ CAN 分析仪│          │ SPI 配置  │                                 │
│  └──────────┘          └──────────┘                                 │
└─────────────────────────────────────────────────────────────────────┘
```

### 2.2 CAN 寻址

| 方向 | CAN ID | 说明 |
|------|--------|------|
| 请求 (CCU → Qi) | `0x18DA0D03` | 物理寻址, 目标=0x0D, 源=0x03 |
| 响应 (Qi → CCU) | `0x18DA030D` | 物理寻址, 目标=0x03, 源=0x0D |

### 2.3 MCU 引脚分配

```
                        AT32F426KBU7-4 (QFN32)
                       ┌─────────────────────┐
              VDD 3.3V │1                32│ PB8 (NC, 输出低)
          HEXT_IN 8MHz │2                31│ BOOT0
         HEXT_OUT 8MHz │3                30│ PB7 (Debug RX)
               M_NRST  │4                29│ PB6 (Debug TX)
              VDDA 3.3V│5                28│ PB5 (NC, 输出低)
        PA0 (霍尔检测) │6                27│ PB4 (NC, 输出低)
          PA1 (NC,低)  │7                26│ PB3 (SIT1145, 输出低)
     PA2 (USART2_TX→Qi)│8                25│ PA15 (NC, 输出低)
     PA3 (USART2_RX←Qi)│9                24│ PA14 (SWCLK)
      PA4 (SPI1_CS)    │10               23│ PA13 (SWDIO)
      PA5 (SPI1_SCK)   │11               22│ PA12 (CAN_TX)
      PA6 (SPI1_MISO)  │12               21│ PA11 (CAN_RX)
      PA7 (SPI1_MOSI)  │13               20│ PA10 (NC, 输出低)
          PB0 (NC,低)  │14               19│ PA9  (NC, 输出低)
  PB1 (12V_Buck,低有效)│15               18│ PA8  (NC, 输出低)
  PB2 (5V_Qi,高有效)   │16               17│ PB10 (NC, 输出低)
                       └─────────────────────┘
```

### 2.4 系统模块关系

```
┌─────────────────────────────────────────────────────────────┐
│                    Qi 无线充电模块                            │
│                                                             │
│  ┌───────────────────────────────────────────────────────┐  │
│  │              Bootloader (28KB @ 0x08000000)            │  │
│  │                                                       │  │
│  │  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌─────────┐  │  │
│  │  │boot_jump │ │boot_safe │ │boot_trial│ │boot_meta│  │  │
│  │  │  跳转控制 │ │  Safe    │ │  Trial   │ │ Metadata│  │  │
│  │  │          │ │  Mode    │ │  Boot    │ │ 管理    │  │  │
│  │  └──────────┘ └──────────┘ └──────────┘ └─────────┘  │  │
│  │  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌─────────┐  │  │
│  │  │boot_verify│ │  isotp   │ │sit1145   │ │can_drv  │  │  │
│  │  │ CRC/ECDSA│ │ ISO-TP   │ │CAN 收发器│ │CAN 驱动 │  │  │
│  │  └──────────┘ └──────────┘ └──────────┘ └─────────┘  │  │
│  │  ┌──────────┐ ┌──────────┐                           │  │
│  │  │  sha256  │ │  uECC    │                           │  │
│  │  │  哈希    │ │ ECDSA签名│                           │  │
│  │  └──────────┘ └──────────┘                           │  │
│  └───────────────────────────────────────────────────────┘  │
│                           │ 跳转                             │
│  ┌────────────────────────▼──────────────────────────────┐  │
│  │           Application (42KB/Slot @ +0x100)             │  │
│  │                                                       │  │
│  │  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌─────────┐  │  │
│  │  │can_proto │ │ qi_uart  │ │board_gpio│ │lifecycle│  │  │
│  │  │CAN 协议栈│ │Qi 串口   │ │  GPIO    │ │生命周期 │  │  │
│  │  └──────────┘ └──────────┘ └──────────┘ └─────────┘  │  │
│  │  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌─────────┐  │  │
│  │  │ota_trigger│ │device_inf│ │ nvm_drv  │ │  isotp  │  │  │
│  │  │OTA 触发  │ │设备信息  │ │NVM 驱动  │ │ ISO-TP  │  │  │
│  │  └──────────┘ └──────────┘ └──────────┘ └─────────┘  │  │
│  └───────────────────────────────────────────────────────┘  │
│                                                             │
│  ┌───────────────────────────────────────────────────────┐  │
│  │                共享 Flash 区域                          │  │
│  │  Metadata (主+备) │ Device Info (SN) │ NVM Config      │  │
│  └───────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

---

## 3. 核心特性

### 3.1 双槽 A/B Ping-Pong 升级

```
出厂状态:                    第一次 OTA:                 第二次 OTA:
┌──────────┐               ┌──────────┐               ┌──────────┐
│ Slot A   │ ← 活跃        │ Slot A   │ ← 旧版        │ Slot A   │ ← 新版(活跃)
│ (v1.0)   │               │ (v1.0)   │               │ (v1.1)   │
├──────────┤               ├──────────┤               ├──────────┤
│ Slot B   │ ← 空          │ Slot B   │ ← 新版(活跃)  │ Slot B   │ ← 旧版
│ (空)     │               │ (v1.1)   │               │ (v1.0)   │
└──────────┘               └──────────┘               └──────────┘
```

- 只擦写 **非活跃槽**，活跃槽始终保留可回退
- 掉电安全：先写 Metadata 备份副本，再写主副本（先备后主）

### 3.2 Trial Boot 试运行机制

```
OTA 下载完成 → 复位 → Bootloader 选择新槽
                              │
                    ┌─────────▼──────────┐
                    │  Trial Boot (10s)   │
                    │  新槽 APP 运行      │
                    │                     │
                    │  APP 健康检查通过?   │
                    │  ├─ 是 → 确认       │
                    │  │       active=新槽 │
                    │  │       (永久生效)  │
                    │  └─ 否 → 超时复位   │
                    │         回滚旧槽    │
                    └─────────────────────┘
```

状态流转: `IDLE → PENDING(OTA写入完成后) → ACTIVE(APP启动后) → CONFIRMED(APP确认后)`

### 3.3 安全机制

| 安全层 | 实现 |
|--------|------|
| 镜像签名 | ECDSA P-256, 64字节 R‖S (IEEE P1363) |
| 完整性校验 | CRC32 (IEEE 802.3) + SHA-256 哈希 |
| 安全访问 | UDS 0x27 SecurityAccess, Seed(32B) → SHA-256 → ECDSA 签名验签 |
| 防回滚 | 版本号校验 (XATO 头 version 字段) |
| 掉电保护 | Metadata 双副本 (先写备后写主) |
| Device Info 保护 | OTA 擦写跳过 0x0801D000~0x0801DFFF, SN 永久保留 |

### 3.4 Safe Mode 救砖

当双槽镜像均无效或 metadata 标记为 `DOWNLOADING` 时，Bootloader 自动进入 Safe Mode：
- 不跳转 APP，停留在 Bootloader
- 开放完整 UDS 下载路径 (0x34/0x36/0x37)
- 支持从空片状态恢复

---

## 4. 仓库目录结构

```
ota-upgrade-of-qi-charger-based-on-can/
│
├── README.md                               ← 本文件
│
├── qi_wireless_bootloader/                 ← Bootloader 工程
│   ├── mdk_project/                        ← Keil 工程文件 (.uvprojx)
│   ├── mdk_user/
│   │   ├── Inc/                            ← 时钟、中断配置头文件
│   │   └── Src/
│   │       └── main.c                      ← 入口: bootloader_main()
│   ├── mdk_can/
│   │   ├── Inc/can_driver.h
│   │   └── Src/can_driver.c               ← CAN 底层驱动
│   ├── mdk_app/
│   │   ├── Inc/
│   │   │   ├── boot_jump.h                 ← 跳转控制
│   │   │   ├── boot_metadata.h             ← Metadata 读写
│   │   │   ├── boot_safe_mode.h            ← Safe Mode UDS 下载
│   │   │   ├── boot_trial.h                ← Trial Boot 管理
│   │   │   ├── boot_verify.h               ← CRC/ECDSA 校验
│   │   │   ├── isotp.h                     ← ISO-TP 传输层
│   │   │   ├── sha256.h                    ← SHA-256 哈希
│   │   │   ├── sit1145.h                   ← SIT1145 CAN 收发器驱动
│   │   │   ├── timer_drv.h                 ← 定时器驱动
│   │   │   └── uECC.h                      ← ECDSA P-256 签名库
│   │   └── Src/                            ← 对应 .c 实现
│   └── libraries/
│       ├── cmsis/                          ← ARM CMSIS 核心文件
│       └── drivers/                        ← AT32 SPL 外设驱动库
│
├── qi_wireless_code_slotA/                 ← APP 工程 (Slot A, IROM=0x08007100)
│   ├── mdk_project/
│   ├── mdk_user/
│   │   ├── Inc/
│   │   │   ├── at32f422_426_conf.h         ← 外设模块配置
│   │   │   └── ...
│   │   └── Src/
│   │       └── main.c                      ← APP 入口
│   ├── mdk_can/
│   │   ├── Inc/can_driver.h
│   │   └── Src/can_driver.c
│   └── mdk_app/
│       ├── Inc/
│       │   ├── board_gpio.h                ← GPIO 配置 (霍尔/供电)
│       │   ├── can_protocol.h              ← CAN-UDS 协议处理
│       │   ├── device_info.h               ← 设备信息 (SN 等)
│       │   ├── lifecycle.h                 ← 生命周期状态广播
│       │   ├── nvm_drv.h                   ← NVM 配置存储
│       │   ├── ota_trigger.h               ← OTA 触发 (进入 Boot)
│       │   ├── qi_uart.h                   ← Qi 芯片串口通信
│       │   └── ...
│       └── Src/                            ← 对应 .c 实现
│
├── qi_wireless_code_slotB/                 ← APP 工程 (Slot B, IROM=0x08011900)
│   └── (结构同 Slot A)
│
├── python_tools/                           ← Python 工具集
│   ├── 1.packaging script/                 ← 打包签名子目录
│   │   ├── pack_image_slotA_1_1_1.py       ← Slot A v1.1.1 镜像打包 (XATO 头 + CRC32 + ECDSA)
│   │   ├── pack_image_slotB_1_1_2.py       ← Slot B v1.1.2 镜像打包
│   │   ├── merge_prod_bin.py               ← Boot + APP 合并产线镜像
│   │   ├── verify_image.py                 ← 镜像完整性 + 签名校验
│   │   └── sign_seed.py                    ← SecurityAccess seed 签名 (ECDSA P-256)
│   ├── 2.functional test script/           ← 功能测试子目录
│   │   ├── zcanpro_read_app_version.py     ← 读取 APP 版本
│   │   ├── zcanpro_sn_write.py             ← 写入设备 SN
│   │   ├── zcanpro_charge_start.py         ← 启动充电
│   │   ├── zcanpro_qi_read_status.py       ← 读取 Qi 芯片状态
│   │   ├── zcanpro_qi_read_version.py      ← 读取 Qi 芯片版本
│   │   └── zcanpro_qi_set_power.py         ← 设置 Qi 充电功率
│   ├── iap bin/                            ← Qi 芯片 IAP 固件
│   │   ├── log1.BIN
│   │   └── log2.BIN
│   ├── zcanpro_ext_ota_auto.py             ← 一键 OTA (自动探测入口模式)
│   ├── zcanpro_ext_ota_from_app_auto.py    ← OTA: 从 APP 进 Boot (自动选择 Slot)
│   ├── zcanpro_ext_ota_from_app_slotA_1_1_1.py  ← OTA: 从 APP 进 Boot, 写 Slot A v1.1.1
│   ├── zcanpro_ext_ota_from_app_slotB_1_1_2.py  ← OTA: 从 APP 进 Boot, 写 Slot B v1.1.2
│   ├── zcanpro_ext_ota_from_boot_auto.py   ← OTA: 已在 Boot Safe Mode (自动选择 Slot)
│   ├── zcanpro_ext_ota_from_boot_slotA_1_1_1.py ← OTA: 已在 Boot, 写 Slot A v1.1.1
│   ├── zcanpro_ext_ota_from_boot_slotB_1_1_2.py ← OTA: 已在 Boot, 写 Slot B v1.1.2
│   ├── zcanpro_ext_ota_slotA_1_1_1.py      ← OTA: 兼容旧版一键脚本, 写 Slot A v1.1.1
│   ├── zcanpro_ext_ota_slotB_1_1_2.py      ← OTA: 兼容旧版一键脚本, 写 Slot B v1.1.2
│   ├── zcanpro_qi_iap_log1.py             ← Qi 芯片 IAP 刷写 (log1)
│   ├── zcanpro_qi_iap_log2.py             ← Qi 芯片 IAP 刷写 (log2)
│   └── 脚本使用说明.md
│
└── docs/                                   ← 项目文档
    ├── 1. AT32F426KBU7-4_引脚定义.md
    ├── 2. Flash 分配方案.md
    ├── 4. IAP数据通信协议规范.md            ← MCU ↔ Qi 芯片 UART 协议
    ├── 6. qi_charger_srs_zh.md             ← Qi 充电模块 SRS
    ├── 9. APP镜像打包与产线烧录.md
    ├── 10. CAN-UDS OTA 测试用例表.md        ← 62 条测试用例
    ├── 11. 签名校验与脚本使用.md
    ├── 12. 签名原理与Seed机制.md
    ├── 13. 官方IAP例程vs自定义Bootloader对比.md
    ├── 14. 宏定义切换方式.md
    ├── 15. 计划安排表.md
    ├── 16. 功能需求文档.md
    ├── 合并-CAN协议-UDS-OTA工作流.md
    ├── keys/
    │   ├── private.pem                     ← ECDSA P-256 私钥
    │   └── public.pem                      ← ECDSA P-256 公钥 (烧入 Bootloader)
    └── pdf/                                ← 参考 PDF
```

---

## 5. Flash 空间布局

> **MCU**: AT32F426 — 128KB Flash, 2KB Sector

```
地址             大小      区域                    说明
──────────────────────────────────────────────────────────────────
0x08000000 ┌─────────────────────────────┐
           │     Bootloader (28KB)       │  引导 + Safe Mode + 验签
           │     0x7000 bytes            │
0x08007000 ├─────────────────────────────┤
           │  Slot A: XATO Header (256B) │  magic / length / CRC32
0x08007100 │  Slot A: APP Code (41.75KB) │  ← Keil IROM 入口
           │  0xA700 bytes               │
0x08011800 ├─────────────────────────────┤
           │  Slot B: XATO Header (256B) │  同 Slot A 结构
0x08011900 │  Slot B: APP Code (41.75KB) │  ← OTA 写入目标
           │  0xA700 bytes               │
0x0801C000 ├─────────────────────────────┤
           │  Metadata Primary (2KB)     │  ota_metadata_t (272B)
0x0801C800 ├─────────────────────────────┤
           │  Metadata Backup (2KB)      │  掉电安全副本
0x0801D000 ├─────────────────────────────┤
           │  Device Info (4KB)          │  SN 序列号, OTA 不擦除此区
0x0801E000 ├─────────────────────────────┤
           │  NVM Config (8KB)           │  APP 运行时配置
0x0801FFFF └─────────────────────────────┘
```

### Metadata 结构 (`ota_metadata_t`, 272 字节)

| 偏移 | 字段 | 说明 |
|------|------|------|
| 0x00 | magic | `0x4F54414D` ("MATO") |
| 0x08 | active_slot | 当前活跃槽 (0=A, 1=B) |
| 0x09 | pending_slot | 待试运行槽 (0xFE=无) |
| 0x0A | slot_a_valid | Slot A 镜像有效标志 |
| 0x0B | slot_b_valid | Slot B 镜像有效标志 |
| 0x14 | trial_state | 0=IDLE, 1=PENDING, 2=ACTIVE, 3=CONFIRMED |
| 0x21 | ota_state | 0=IDLE, 1=DOWNLOADING (触发 Safe Mode) |
| 0x10C | crc32 | 上述字段 CRC32 校验 |

### XATO 镜像头 (256 字节)

| 偏移 | 字段 | 说明 |
|------|------|------|
| 0x00 | magic | `0x4F544158` ("XATO") |
| 0x04 | image_length | 固件长度 (不含头) |
| 0x08 | crc32 | 固件 CRC32 校验 |
| 0x0C | signature[64] | ECDSA P-256 签名 (R‖S, IEEE P1363) |
| 0x4C | version[16] | 版本字符串 |
| 0x5C | timestamp | 编译时间戳 |

---

## 6. 协议栈架构

```
┌─────────────────────────────────────────────┐
│        应用层 (Application Layer)            │
│   ISO 14229 — UDS 诊断服务                   │
│   0x10 会话 │ 0x11 复位 │ 0x22 读DID        │
│   0x27 安全 │ 0x2E 写DID │ 0x31 例程        │
│   0x34/36/37/38 下载 │ 0x3E 保活            │
├─────────────────────────────────────────────┤
│        传输层 (Transport Layer)              │
│   ISO 15765-2 — ISO-TP                       │
│   单帧(SF) / 首帧(FF) / 连续帧(CF) / 流控(FC) │
├─────────────────────────────────────────────┤
│        数据链路层 (Data Link Layer)          │
│   CAN 2.0B — 29-bit 扩展帧                   │
│   仲裁 / CRC / 错误检测 / Bus-Off 恢复       │
├─────────────────────────────────────────────┤
│        物理层 (Physical Layer)               │
│   SIT1145 CAN 收发器 (SPI 配置)              │
│   250 kbps / 120Ω 终端 / 差分信号            │
└─────────────────────────────────────────────┘
```

### 关键 UDS 服务

| SID | 名称 | Bootloader | APP |
|-----|------|:----------:|:---:|
| 0x10 | DiagnosticSessionControl | ✅ | ✅ |
| 0x11 | ECUReset | ✅ | ✅ |
| 0x22 | ReadDataByIdentifier | ✅ | ✅ |
| 0x27 | SecurityAccess (ECDSA P-256) | ✅ | ✅ |
| 0x2E | WriteDataByIdentifier | ✅ | ✅ |
| 0x31 | RoutineControl (擦槽) | ✅ | ❌ (NRC 0x11) |
| 0x34 | RequestDownload | ✅ | ❌ (NRC 0x11) |
| 0x36 | TransferData | ✅ | ❌ |
| 0x37 | RequestTransferExit | ✅ | ❌ |
| 0x3E | TesterPresent | ✅ | ✅ |

> APP 不实现 0x34/0x36/0x37，下载只在 Bootloader Safe Mode 下进行。

---

## 7. OTA 升级流程

### 7.1 端到端 OTA 时序

```
  CCU (主机)                          Qi 模块 (Bootloader)
     │                                      │
     │  ── 10 02 (Programming Session) ──►  │
     │  ◄── 50 02 ──────────────────────────│
     │                                      │
     │  ── 27 01 (RequestSeed) ──────────►  │
     │  ◄── 67 01 [seed 32B] ───────────────│
     │                                      │
     │  ── 27 03 [签名分片 ×16] ─────────►  │  SHA256(seed) → ECDSA
     │  ◄── 7F 27 78 (ResponsePending) ─────│  MCU 跑 ECDSA P-256 验签
     │                                      │  (数秒, 非阻塞)
     │  ◄── 67 03 ──────────────────────────│
     │                                      │
     │  ── 27 02 (SendKey/验签) ─────────►  │  MCU 运行 ECDSA P-256 验签
     │  ◄── 7F 27 78 (ResponsePending) ─────│  (数秒, 非阻塞)
     │  ◄── 67 02 (OK) ─────────────────────│
     │                                      │
     │  ── 2E 2010 01 (选 APP) ──────────►  │
     │  ◄── 6E 2010 ────────────────────────│
     │                                      │
     │  ── 31 01 FF00 (擦非活跃槽) ──────►  │  ◄── 7F 31 78 (ResponsePending)
     │  ◄── 71 01 FF00 ─────────────────────│      擦除中...
     │                                      │
     │  ── 34 (RequestDownload) ─────────►  │
     │  ◄── 74 20 01 00 (maxBlock=256) ─────│  上限 256B，实际每帧 128B
     │                                      │
     │  ── 36 [seqNum + 128B 数据] ──────►  │  每帧 128B，重复 N 次
     │  ◄── 76 [seqNum] ────────────────────│
     │                                      │
     │  ── 37 (TransferExit) ────────────►  │  ◄── 7F 37 78 (验签中...)
     │  ◄── 77 ─────────────────────────────│
     │                                      │
     │  ── 11 01 (HardReset) ────────────►  │
     │  ◄── 51 01 ──────────────────────────│
     │                                      │
     │         MCU 复位 → Trial Boot        │
```

### 7.2 APP 触发 OTA

```
APP 收到 OTA 指令 (CAN / 霍尔 / 内部条件)
    │
    ├─ 写 metadata: ota_state = DOWNLOADING
    ├─ NVIC_SystemReset()
    │
    └─ Bootloader 检测到 ota_state == DOWNLOADING
       └─ enter_safe_mode() → 开放 UDS 下载
```

### 7.3 脚本三种入口模式 (ENTRY_MODE)

OTA 脚本支持三种启动模式，通过 `ENTRY_MODE` 变量控制：

| 模式 | 脚本 | 行为 |
|------|------|------|
| `auto` (默认) | `zcanpro_ext_ota_auto.py` | 探测 0x34（RequestDownload）。若收到 NRC 0x11（ServiceNotSupported），视为当前在 APP → 自动执行 `10 02 → 27 01/03/02 → 11 01` 进入 Boot Safe Mode。若 0x34 直接响应 74，则已在 Boot，跳过切换。 |
| `app` | `zcanpro_ext_ota_from_app_*.py` | 强制从 APP 进入 Boot：`10 02 → 27 01/03/02 → 11 01 → 等待 Boot Safe Mode` |
| `boot` | `zcanpro_ext_ota_from_boot_*.py` | 已在 Boot Safe Mode，直接开始下载：`10 02 → 27 01/03/02 → 34/36/37` |

```
                          ┌─────────────────┐
                          │ ENTRY_MODE 判断  │
                          └────────┬────────┘
                                   │
                  ┌────────────────┼────────────────┐
                  │                │                │
              auto/app            boot            (无)
                  │                │                │
          ┌───────▼──────┐  ┌─────▼──────┐   ┌────▼──────┐
          │ 发 0x34 探测  │  │ 已在 Boot  │   │ 默认 auto │
          │ NRC 0x11?    │  │ 直接下载   │   │          │
          │ 是→APP→切Boot│  │            │   │          │
          └──────────────┘  └────────────┘   └──────────┘
```

### 7.4 OTA 关键机制

**非活跃槽擦写**

OTA 过程只擦写非活跃槽，活跃槽始终保留可回退。通过 `2E 2010` DID 选择目标 Slot 后，`31 01 FF00` 仅擦除非活跃槽 Flash。

**掉电安全（Metadata 双副本）**

Metadata 区域包含主副本 (`0x0801C000`) 和备份副本 (`0x0801C800`)。写入顺序：先写备份 → 校验通过 → 再写主副本。掉电发生在写主副本过程中时，Bootloader 可从备份恢复。

**Trial Boot 试运行**

OTA 下载完成后写入 metadata `trial_state = PENDING`，MCU 复位后 Bootloader 选择新槽启动。APP 启动后 `trial_state → ACTIVE`，APP 确认健康后 `→ CONFIRMED`（永久生效）。若 10s 窗口内未确认，超时复位回滚旧槽。

**镜像验签**

`37 TransferExit` 时 Bootloader 执行 ECDSA P-256 签名验证（64 字节 R‖S P1363 格式）+ CRC32 校验。MCU 侧 ECDSA 验签需数秒，期间回 `7F 37 78`（ResponsePending）。

**防回滚**

Bootloader 在 `select_boot_slot()` 时校验 XATO 头中的版本号字段，拒绝降级镜像。

---

## 8. Bootloader 启动流程

```
上电 / 复位
  │
  ├─ system_clock_config()          ← 180 MHz
  ├─ nvic_priority_group_config()
  ├─ timer_drv_init()
  ├─ boot_metadata_init()           ← 读主区 → 备份 → 默认值
  ├─ detect_boot_reason()           ← 上电/软复位/WDG/OTA/回滚
  │
  ├─ ota_state == DOWNLOADING? ──是──▶ enter_safe_mode() (不返回)
  │
  ├─ process_trial_state()          ← PENDING→ACTIVE, 超限回滚
  ├─ select_boot_slot()             ← PENDING/ACTIVE→trial_slot, 否则→active_slot
  │
  ├─ try_boot_slot(选中槽)           ← 验签 XATO 头 + ECDSA
  │     ├─ 通过 → jump_to_app()
  │     └─ 失败 → try_boot_slot(另一槽)
  │           ├─ 旧槽成功 → 写回滚 metadata → jump
  │           └─ 双槽失败 → enter_safe_mode()
  │
  └─ enter_safe_mode()              ← Safe Mode: 完整 UDS 下载循环
```

---

## 9. Python 工具集

### 9.1 打包与签名工具

位于 `python_tools/1.packaging script/` 子目录。

| 脚本 | 功能 | 依赖 |
|------|------|------|
| `pack_image_slotA_1_1_1.py` | 裸 bin → XATO 头 .ota.bin (Slot A v1.1.1, CRC32 + ECDSA P-256) | 标准库 |
| `pack_image_slotB_1_1_2.py` | 裸 bin → XATO 头 .ota.bin (Slot B v1.1.2, CRC32 + ECDSA P-256) | 标准库 |
| `merge_prod_bin.py` | Bootloader + Slot A 合并为单文件产线镜像 | 标准库 |
| `verify_image.py` | 校验 XATO 镜像完整性 + 签名 | 标准库 |
| `sign_seed.py` | SecurityAccess seed 签名生成 (ECDSA P-256) + CAN 帧输出 | `cryptography` |

### 9.2 OTA 脚本

位于 `python_tools/` 根目录。根据入口模式和目标 Slot 选择对应脚本：

| 脚本 | 说明 |
|------|------|
| `zcanpro_ext_ota_auto.py` | 一键 OTA (自动探测 APP/Boot 入口) |
| `zcanpro_ext_ota_from_app_auto.py` | 从 APP 进 Boot → 下载 (自动选 Slot) |
| `zcanpro_ext_ota_from_app_slotA_1_1_1.py` | 从 APP 进 Boot → 写 Slot A v1.1.1 |
| `zcanpro_ext_ota_from_app_slotB_1_1_2.py` | 从 APP 进 Boot → 写 Slot B v1.1.2 |
| `zcanpro_ext_ota_from_boot_auto.py` | 已在 Boot Safe Mode → 下载 (自动选 Slot) |
| `zcanpro_ext_ota_from_boot_slotA_1_1_1.py` | 已在 Boot → 写 Slot A v1.1.1 |
| `zcanpro_ext_ota_from_boot_slotB_1_1_2.py` | 已在 Boot → 写 Slot B v1.1.2 |

### 9.3 Qi 芯片 IAP 脚本

| 脚本 | 说明 |
|------|------|
| `zcanpro_qi_iap_log1.py` | Qi 芯片 IAP 刷写 (固件包 1) |
| `zcanpro_qi_iap_log2.py` | Qi 芯片 IAP 刷写 (固件包 2) |

### 9.4 功能测试脚本

位于 `python_tools/2.functional test script/` 子目录。

| 脚本 | 功能 |
|------|------|
| `zcanpro_read_app_version.py` | 读取 APP 版本 (DID 0xF195) |
| `zcanpro_sn_write.py` | 写入设备 SN |
| `zcanpro_charge_start.py` | 启动充电 |
| `zcanpro_qi_read_status.py` | 读取 Qi 芯片状态 |
| `zcanpro_qi_read_version.py` | 读取 Qi 芯片版本 |
| `zcanpro_qi_set_power.py` | 设置 Qi 充电功率 |

### 快速使用

```bash
# 打包镜像 (需先切换到 packaging script 子目录)
cd "python_tools/1.packaging script"
python pack_image_slotA_1_1_1.py \
  --bin ../../qi_wireless_code_slotA/mdk_project/Objects/qi_wireless.bin \
  --key ../../docs/keys/private.pem \
  --out ../../qi_wireless_code_slotA/mdk_project/Objects/qi_wireless.ota.bin

# 合并产线镜像
python merge_prod_bin.py \
  --boot ../../qi_wireless_bootloader/mdk_project/Objects/qi_wireless.bin \
  --app  ../../qi_wireless_code_slotA/mdk_project/Objects/qi_wireless.ota.bin \
  --out  ../../prod_image.bin

# 校验镜像 (回到仓库根目录)
cd ../..
python python_tools/1.packaging\ script/verify_image.py \
  --bin qi_wireless_code_slotA/mdk_project/Objects/qi_wireless.ota.bin \
  --pub docs/keys/public.pem

# 一键 OTA (需 ZCANPRO + python-can)
python python_tools/zcanpro_ext_ota_auto.py
```

---

## 10. 构建与烧录

### 10.1 编译环境

| 工具 | 版本要求 |
|------|----------|
| Keil MDK | v5.38+ |
| AT32 IDE Pack | AT32F426 支持包 |
| Python | 3.8+ (工具脚本) |

### 10.2 Bootloader 编译

1. 打开 `qi_wireless_bootloader/mdk_project/qi_wireless.uvprojx`
2. Target → IROM1: `0x08000000` / `0x7000`
3. Build → 输出 `qi_wireless.bin`

### 10.3 APP 编译 (Slot A)

1. 打开 `qi_wireless_code_slotA/mdk_project/qi_wireless.uvprojx`
2. Target → IROM1: `0x08007100` / `0xA700`
3. Linker → 勾选 "Use Memory Layout from Target Dialog"
4. Build → 输出 `qi_wireless.bin` (裸 bin，不含 XATO 头)

### 10.4 产线烧录

```bash
# 1. 打包 APP 镜像
cd "python_tools/1.packaging script"
python pack_image_slotA_1_1_1.py \
  --bin ../../qi_wireless_code_slotA/mdk_project/Objects/qi_wireless.bin

# 2. 合并 Boot + APP
python merge_prod_bin.py

# 3. 烧录合并后的 prod_image.bin 到 0x08000000
# (使用 J-Link / AT-Link / SWD)
```

> **注意**: 产线只烧 Bootloader + Slot A。Slot B 出厂为空，留给首次 CAN OTA 写入。

---

## 11. 文档索引

| 编号 | 文档 | 内容 |
|------|------|------|
| 1 | [引脚定义](docs/1.%20AT32F426KBU7-4_引脚定义.md) | AT32F426KBU7-4 QFN32 全引脚功能定义 |
| 2 | [Flash 分配方案](docs/2.%20Flash%20分配方案.md) | 128KB Flash 分区、Metadata 结构、XATO 头 |
| 4 | [IAP 数据通信协议](docs/4.%20IAP数据通信协议规范.md) | MCU ↔ Qi 芯片 UART 通信协议 |
| 6 | [Qi 充电模块 SRS](docs/6.%20qi_charger_srs_zh.md) | 软件需求规格说明书 |
| 9 | [镜像打包与产线烧录](docs/9.%20APP镜像打包与产线烧录.md) | 打包脚本用法、Keil IROM 配置 |
| 10 | [OTA 测试用例表](docs/10.%20CAN-UDS%20OTA%20测试用例表.md) | 62 条测试用例 (P0/P1/P2) |
| 11 | [签名校验与脚本](docs/11.%20签名校验与脚本使用.md) | 签名工具使用说明 |
| 12 | [签名原理与 Seed 机制](docs/12.%20签名原理与Seed机制.md) | ECDSA P-256 + SHA-256 原理 |
| 13 | [官方 IAP 例程 vs 自定义 Bootloader](docs/13.%20官方IAP例程vs自定义Bootloader对比.md) | 官方 IAP 方案与本项目 Bootloader 对比 |
| 14 | [宏定义切换方式](docs/14.%20宏定义切换方式.md) | 编译宏配置与功能切换 |
| 15 | [计划安排表](docs/15.%20计划安排表.md) | 项目开发计划 |
| 16 | [功能需求文档](docs/16.%20功能需求文档.md) | 功能需求清单 |
| — | [CAN 协议-UDS-OTA 工作流](docs/合并-CAN协议-UDS-OTA工作流.md) | CAN 协议与 UDS OTA 完整工作流 |

---

## 12. 功能开发状态

### 12.1 已实现并跑通

**Bootloader**

- Safe Mode 全量 UDS 服务: 0x10 (会话控制) / 0x11 (复位) / 0x22 (读DID) / 0x27 (安全访问) / 0x31 (例程控制) / 0x34 (请求下载) / 0x36 (数据传输) / 0x37 (传输退出) / 0x3E (保活)
- ECDSA P-256 签名验签 (uECC 库)
- Trial Boot 试运行管理 (PENDING → ACTIVE → CONFIRMED, 10s 窗口)
- Metadata 双备份掉电保护
- Bus-Off 恢复机制
- CAN 采样点 75% (BTS1=54, BTS2=18, 18MHz÷72Tq)

**APP 侧**

- UDS 诊断服务 + OTA 触发 (写 DOWNLOADING 标记 → 复位进 Boot)
- DID 读写 (版本/SN/配置等)
- 生命周期 CAN 状态广播
- Qi 芯片 IAP 支持 (DID 0x2130~0x2133, UART 0xCC 协议)
- SN 序列号存储
- 充电基本控制 (使能/禁用/功率限值 NVM 持久化)

**Python 工具链**

- 镜像打包: `pack_image_slotA_1_1_1.py` / `pack_image_slotB_1_1_2.py`
- 镜像合并: `merge_prod_bin.py`
- 镜像校验: `verify_image.py`
- 签名工具: `sign_seed.py`
- 一键 OTA: `zcanpro_ext_ota_auto.py` + from_app/from_boot 变体
- Qi IAP: `zcanpro_qi_iap_log1.py` / `log2.py`
- 功能测试脚本 (版本读取/SN写入/启停充电等)
- **端到端 MCU OTA 已验证通过**

### 12.2 验证中（当前红项）

| 项 | 说明 |
|----|------|
| 0x31 擦除 + 0x37 TransferExit 非阻塞 0x78/0x77 处理 | 真实总线压力下的 Bus-Off 稳定性回归验证 |

对应计划 A1.6，是当前 MCU OTA 链路唯一未关闭项，需在真实 CAN 总线压力测试中确认无遗漏。

### 12.3 待实现 / 待完善

| 类别 | 项 | 说明 |
|------|------|------|
| 充电状态机 | 11 态 (M6) | 依赖 Qi 芯片接口完善 |
| 故障处理 | 热管理 / FOD / 硬件故障 (E1~E7) | 待实现 |
| 生命周期广播 | 完整字节格式与事件驱动 (D3/D5/D6) | 当前为简化版 |
| 低功耗管理 | H1~H8 | 当前仅 SIT1145 收发器级 Standby |
| | | UDS 空闲 180s → `can_lp_enter_standby` → 发 SHUTDOWN 标记 `06 41 53 42` → SIT1145 切 Standby 监听 |
| | | 唤醒靠总线活动拉低 PA11 → `can_lp_enter_normal` |
| | | MCU 主循环纯轮询，无 WFI/Stop，MCU 本身未休眠 |
| | | 超时 180s 为硬编码 (`CAN_LP_IDLE_TIMEOUT_MS`)，非 SRS 要求的 DID 0x2117 可配（该 DID 未实现） |
| UDS 业务流程 | F1~F6 | 待实现 |

### 12.4 版本现状

| 组件 | 版本 |
|------|------|
| Slot A APP | QC_JYF_FW_1.1.1 |
| Slot B APP | QC_JYF_FW_1.1.2 |
| Bootloader | QC_JYF_BL_1.0.0 |
| 硬件版本 | QC_JYF_HW_1.1.5 |

---

## 许可证

本项目为内部开发项目，未经授权不得外传。

---

> **Lime 固件团队** — 2026
