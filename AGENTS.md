# 仓库代理规则

## 唯一改动源规则

- 所有改动（代码、文档、脚本）一律先在 WSL 侧仓库 `\\wsl.localhost\Ubuntu-24.04\home\whites\embedded_item\ota-upgrade-of-qi-charger-based-on-can`（即 `/home/whites/embedded_item/ota-upgrade-of-qi-charger-based-on-can`）中进行。
- 改完必须 `git push` 到 origin/main，其他位置的 clone（如 Windows 端 `C:\Users\18452\Documents\Github-young-nights\...`）只通过 `git pull` 同步，禁止在非 WSL 仓库直接修改后当源使用。
- 背景教训：曾出现 Windows 端 clone 本地改动未推送、与 WSL 仓库分叉，导致修复互相覆盖、无法互相验证（2026-09 OTA 唤醒探测修复事件）。

### 修改范围边界（2026-09-19 起）

- 用户指令原文（2026-09-19 10:35，om_x100b65eb063698a0b2a49fbeb40a75c）：「\\wsl.localhost\Ubuntu-24.04\home\whites\embedded_item\ota-upgrade-of-qi-charger-based-on-can 你只负责WSL2下面的这个工程的修改就行，Windows下的不要去动，写入此路径下AGENTS.md中把这个规则」。
- **修改范围仅限**：WSL2 工程本仓库 `\\wsl.localhost\Ubuntu-24.04\home\whites\embedded_item\ota-upgrade-of-qi-charger-based-on-can`（即 `/home/whites/embedded_item/ota-upgrade-of-qi-charger-based-on-can`）。本规则为上文「唯一改动源规则」的强化版：从「不得以 Windows 为源修改」升级为「Windows 端一律不触碰」。
- **Windows 端一律不触碰**：`I:\GitHub-young-nights`（用户运行端 clone）、`C:\Users\18452\Documents\Github-young-nights`（旧 clone）等一切 Windows 路径——不修改 / 不写入 / 不删除 / 不执行构建；运行端更新一律由用户自行 `git pull`。
- **例外**：Windows 端操作仅在用户对具体操作明确指令时执行（例：2026-09-18 23:04 编译产物清理属单次授权）；未明确指令时默认禁止。
- **代理任务书纪律**：所有 AI 代理（coder / clerk / evaluator 等）的任务书必须内置本约束声明，任务书未声明时以本 AGENTS.md 条目为准。

## 提交推送规则

- 每次代码修改后，强制 `git add -A` 全工程提交，不留残留文件。
- 自动流程：`git pull` → edit → `git add -A` → `git commit` → `git push`。
- 仓库下所有变更（代码、文档、配置）必须一并提交推送。
- **提交说明格式**：commit message 的 summary 简洁概括，但必须在 Description 中详细描述变更内容，按模块/文件分点列出具体改动，包含关键地址、常量、新增函数名等技术细节。示例：
  - 1. Device Info 结构扩展（device_info.h/c）：新增 ecdsa_pubkey[65] 存储 SEC1 未压缩公钥...
  - 2. Bootloader 读公钥逻辑（boot_verify.c）：优先从 Device Info 读取...
  - 3. APP UDS 服务（can_protocol.c/h）：新增 DID 0x2120 读写公钥...

## 版本号联动规则（2026-09-18 起）

- 软件版本号**唯一定义**在 APP 固件编译常量 `SW_VERSION_STR`（`qi_wireless_code_app/mdk_app/Src/can_protocol.c`）。
- 发版只改两处：固件 `SW_VERSION_STR` + 文档（docs/README 中的版本值与说明）。
- 打包产物 XATO 镜像头 version 区（偏移 0x4C，16 字节）打包固定填 `0x00`，**不携带版本号**；打包/OTA/校验脚本不写、不读、不输出该字段。
- 不存在打包脚本 `IMAGE_VERSION` 常量环节（已删除）；UDS DID `0xF195` 应答取固件编译常量，不读镜像头 / OTA metadata。
- 独立版本族，不随 APP 版本联动：`BOOTLOADER_VER_STR`、`HW_VERSION_STR`、IAP log1/log2 的 `EXPECTED_FW_VERSION`（Qi 芯片固件）。

## log1 / log2 脚本同步规则

- `zcanpro_qi_iap_log1.py` 和 `zcanpro_qi_iap_log2.py` 是同一套 IAP 脚本的不同固件版本副本。
- **除以下 4 项外，两个文件必须完全一致**：
  1. 文件头注释中的固件文件名（log1.BIN / log2.BIN）
  2. `FIRMWARE_NAME` 常量
  3. `EXPECTED_FW_VERSION` 常量
  4. 文件头注释中的互斥说明
- **修改任一脚本的 IAP 逻辑后，必须同步另一个脚本**。检查方式：
  ```bash
  diff python_tools/zcanpro_qi_iap_log1.py python_tools/zcanpro_qi_iap_log2.py
  ```
  diff 输出应仅包含上述 4 项差异，不得有其他不同。
- 提交时在 commit message 中注明 `log1 + log2 已同步`。
