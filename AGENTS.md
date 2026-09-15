# 仓库代理规则

## 提交推送规则

- 每次代码修改后，强制 `git add -A` 全工程提交，不留残留文件。
- 自动流程：`git pull` → edit → `git add -A` → `git commit` → `git push`。
- 仓库下所有变更（代码、文档、配置）必须一并提交推送。
- **提交说明格式**：commit message 的 summary 简洁概括，但必须在 Description 中详细描述变更内容，按模块/文件分点列出具体改动，包含关键地址、常量、新增函数名等技术细节。示例：
  - 1. Device Info 结构扩展（device_info.h/c）：新增 ecdsa_pubkey[65] 存储 SEC1 未压缩公钥...
  - 2. Bootloader 读公钥逻辑（boot_verify.c）：优先从 Device Info 读取...
  - 3. APP UDS 服务（can_protocol.c/h）：新增 DID 0x2120 读写公钥...

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
