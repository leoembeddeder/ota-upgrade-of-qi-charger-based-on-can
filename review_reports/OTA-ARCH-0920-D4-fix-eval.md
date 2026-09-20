# OTA-ARCH-0920 · D4 审计修复二次审查报告

- **审查对象**：commit `681a4d1`（已推送 origin/main），基于 `7af8bcf` 的修复交付
- **改动文件**：`python_tools/1.packaging script/verify_image.py`（+32/−10）、`python_tools/1.packaging script/merge_prod_bin.py`（+21/−…）
- **审查类型**：只读二次审查（不改码），静态分析 + 运行时验证（合成签名镜像 + 场景化 CLI 测试）
- **审查时间**：2026-09-21 04:33–04:45 CST
- **结论**：**PASS_WITH_NOTES**（置信度 92/100）

> 说明：coder 自报的 4 项修复（M1① M1② R1 R2）全部核实为**真实落地且行为正确**，运行时证据齐全。
> 附加发现 1 项**范围外**既有缺陷（`--hex` 路径崩溃，旧版本同样存在，非本次交付引入）与 3 项轻微/信息级备注，均不阻塞放行。

---

## 1. M1① 公钥路径（openssl 验签 key 文件写入改写）— ✅ PASS

| 审查点 | 结论 | 证据 |
|---|---|---|
| diff：公钥改为 PEM 原文直写 | ✅ | `verify_image.py:141` `f_pub.write(open(public_key_path,'rb').read())`；旧版 `f_pub.write(b"\x04"+sec1[1:] …)` 已删除（git diff 7af8bcf..681a4d1 hunk 3） |
| PRIVATE 兜底逻辑正确 | ✅ | `verify_image.py:129` 读 key_raw；`:130` `if b"PRIVATE" in key_raw:`；`:132-135` `subprocess.call(["openssl","pkey","-in",public_key_path,"-pubout","-out",pub_path], …)`；`:137-139` `if rc_pub != 0:` 兜底 `fh.write(key_raw)` 原文直写 |
| pem_to_sec1 标注 DEPRECATED | ✅ | `verify_image.py:72` docstring 含 `DEPRECATED (D4-M1) … do not use on signing/verification paths`；全仓 grep `pem_to_sec1` 仅剩定义点 `:69`，无任何调用点（含 verify_ecdsa 内已移除） |
| 无其他调用点被误改 | ✅ | 仓库 `*.py` 全量 grep：`pem_to_sec1` 唯一命中 = 定义处；`p1363_to_der`（仍在用，验签 DER 转换）未被改动 |
| 临时文件生命周期 | ✅（附信息级备注） | 创建：`:112-114` 三个 `NamedTemporaryFile(delete=False)`；清理：`:145-146` `for p in (sig_path,pub_path,dgst_path): os.unlink(p)`；**实测 6 次 verify 运行前后 /tmp tmp\* 计数 6→6，零泄漏**。备注 N4：若异常发生在创建与 unlink 之间则泄漏——此为旧版既有模式，本 commit 未恶化 |
| 运行时验证 `--key private.pem` | ✅ | 合成镜像（openssl `dgst -sha256 -sign` 用 `docs/keys/private.pem` 签名，DER 解析回 R\|\|S 64B 放入 header @0x0C）：`verify_image.py <img> --key docs/keys/private.pem` → `ecdsa: OK openssl exit 0`，`RESULT: PASS`，rc=0（TEST 2） |
| 运行时验证 `--key public.pem` | ✅ | 同镜像 + `docs/keys/public.pem` → `ecdsa: OK openssl exit 0`，`RESULT: PASS`，rc=0（TEST 1）。keypair 匹配性已预验证：`openssl pkey -in private.pem -pubout` ≡ `public.pem`（diff 空） |

**关键判定**：public PEM 直写路径与 private→`pkey -pubout` 派生路径**均实测打通**（OpenSSL 3.0.13，与 coder 声称的宿主环境一致）。

## 2. M1② 哈希路径（双重哈希消除）— ✅ PASS

| 审查点 | 结论 | 证据 |
|---|---|---|
| diff：`f_dgst.write(firmware)` | ✅ | `verify_image.py:121` `f_dgst.write(firmware)`；旧版 `digest = hashlib.sha256(firmware).digest()` + `f_dgst.write(digest)` 已删除 |
| openssl dgst 调用参数正确 | ✅ | `verify_image.py:142-143` `["openssl","dgst","-sha256","-verify",pub_path,"-signature",sig_path,dgst_path]` —— 参数顺序/语义正确（dgst 自行对 dgst_path 文件内容做单次 SHA256） |
| 无 hashlib 残留 | ✅ | `import hashlib` 已从 import 块删除（`verify_image.py:15-20` 仅 argparse/binascii/os/struct/sys）；全文件 grep `hashlib`/`.digest()` 零命中，仅注释文字提及 SHA256（`:118-119`） |
| 双重哈希消除的**行为级证明** | ✅ | TEST 1 签名 = `openssl dgst -sha256 -sign`（固件**单次**哈希）；若脚本仍双重哈希，`dgst -verify` 必然失败。实测 `ecdsa: OK exit 0` ⇒ 验签域与签名域一致 ⇒ 单次哈希路径确认 |
| 负向对照 | ✅ | 篡改固件 1 字节（TEST 3）：`crc32 FAIL` + `ecdsa: FAIL openssl exit 1` + `RESULT: FAIL` rc=1 —— 证明该检查真实生效，非恒真 |

## 3. R1 @0x4C 非全零 WARNING — ✅ PASS

| 审查点 | 结论 | 证据 |
|---|---|---|
| 判断逻辑正确 | ✅ | `verify_image.py:189` `if reserved_ver != b"\x00" * 16:` —— 16 字节整段精确比较（等效逐字节）；`reserved_ver = header[0x4C:0x4C+16]`（`:186`，HDR_RESERVED_VER_OFF=0x4C） |
| 不影响 PASS/FAIL | ✅ | `verify_image.py:199` `all_ok = ok_magic and ok_len and ok_crc and ok_reset and (ok_sig is not False)` —— WARNING 条件**不在**判定链中；实测（TEST 4）reserved=0xAA×16 镜像：打印 WARNING，`RESULT: PASS` rc=0，ecdsa 仍 OK（签名域仅覆盖固件，不含 header） |
| 输出格式含偏移提示 | ✅ | `verify_image.py:190` `WARNING: hdr_reserved_ver @0x4C not all-zero (expected 0x00)` —— 含偏移 @0x4C 与期望值；紧随 `:187` `hdr @0x4C : reserved placeholder = …` 原值打印，可定位 |

## 4. R2 merge_prod_bin.py XATO 硬闸 — ✅ PASS

| 审查点 | 结论 | 证据 |
|---|---|---|
| 默认 ERROR + sys.exit(1) | ✅ | `merge_prod_bin.py:102` 判定 `len(app_data)<4 or app_data[:4]!=b"XATO"`（未改动）；`:112-116` ERROR 文案（含 `Use --force to merge anyway.`）+ `:116` `sys.exit(1)`。实测 TEST A：rc=1，stderr=ERROR 行，**且 out 文件未生成**（闸在 merge/写盘之前） |
| --force 实现正确 | ✅ | argparse `:79-81` `parser.add_argument("--force", action="store_true", help=…)`；逻辑分支 `:106` `if args.force:` → `:107-110` WARNING 文案（含 `continuing because --force was given`），不 exit。实测 TEST B：rc=0 + stderr WARNING + 产物写出 |
| 合法 XATO 路径不受影响 | ✅ | 实测 TEST C（XATO 头镜像、默认无 --force）：rc=0，**stderr 空**（无任何 magic 提示）；TEST D（XATO + --force）：rc=0，stderr 亦空 —— force 不扰动正常路径 |
| 合并产物字节级正确 | ✅ | Python 断言：`out = boot(100B) + 0xFF×(0x4000−100) + app`，app 落位 @0x4000，总长 0x4000+len(app)；XATO 用例 `d[0x4000:0x4004]==b"XATO"`，非 XATO 用例 payload 原样 —— **3 个产物全部断言通过** |
| <4B 短 app 硬闸 | ✅ | TEST E（2 字节 app）：默认 rc=1 + ERROR（命中 `len(app_data)<4` 分支） |
| 不改偏移/magic/常量 | ✅ | 常量行 old-vs-new 值逐一 diff：`BOOT_BASE=0x08000000 / BOOT_SIZE=0x4000 / APP_SIZE=0xC000 / APP_OFFSET / APP_BASE` **完全一致**（仅行号因 docstring +1 偏移）；verify_image.py 侧 `IMAGE_*/APP_*/BACKUP_*/HDR_*` 常量同样值级一致 |

## 5. 横向检查 — ✅ PASS

| 审查点 | 结论 | 证据 |
|---|---|---|
| 未碰固件/pack_image.py/zcanpro_ext_ota_auto.py | ✅ | `git diff 7af8bcf..681a4d1 --name-only` 仅列 2 个 python_tools 脚本文件；`git show --name-only 681a4d1` 同 |
| py_compile 两文件 | ✅ | `python3 -m py_compile` 两文件通过，无输出 |
| pyflakes 两文件 | ✅ | pyflakes 3.4.0（Python 3.12.3）两文件零输出 = clean |
| 无新增 import 依赖 | ✅ | verify_image.py：仅**删除** `import hashlib`，无新增（subprocess/tempfile 仍为函数内局部 import，旧版即如此）；merge_prod_bin.py import 块零变化 |
| 改动范围与任务书一致 | ✅ | diff 三个 hunk 精确对应 M1①②（verify_ecdsa 重写）、R1（WARNING）、R2（硬闸+--force）+ 配套 docstring/help；无越权改动 |

## 6. coder 提及的工作树异常核查 — ✅ 已确认与本 commit 无关

- `git status --short`：`review_reports/` 下 10 个 `*.md` 显示 ` D`（工作树未暂存删除）：OTA-ARCH-0920-D2-log-audit / D3-fix-eval / D3-full-audit / core-eval / core-review / fix-eval / q2-eval / q2-review / r3-script-review / README.md
- **`git show --name-only 681a4d1` 不包含上述任何文件** —— commit 仅含 2 个脚本文件，删除系工作树本地状态（历史提交中文件仍在，可随时 `git checkout -- review_reports/` 恢复），与本次交付无关
- 当前 `review_reports/` 盘上仅存 `OTA-ARCH-0920-D4-pack-coupling-audit.md`（7af8bcf 提交的 D4 审计报告，未被删）与本报告

---

## 7. 备注项（均不阻塞）

### N1【范围外·既有缺陷·建议跟进】`--hex` 路径 TypeError 崩溃
- 现象：`merge_prod_bin.py --hex` → `bin_to_ihex` 在 `merge_prod_bin.py:52`（及同类 `:61`）抛 `TypeError: unsupported operand type(s) for +: 'int' and 'bytes'`。`bytes.fromhex()` 产生 bytes 对象，`sum(int 起始 + bytes)` 不可加。
- **既有性举证**：`bin_to_ihex` 函数体 old(7af8bcf)-vs-new(681a4d1) diff = **完全一致**（本 commit 未触碰该函数，diff 三个 hunk 亦不含它）；用 `git show 7af8bcf` 版本实跑同样崩溃（line 51 同一行）⇒ **非 681a4d1 引入的回归**，属此前既有缺陷，本 commit 合理地未纳入范围。
- 影响：bin 合并在 hex 转换**之前**已完成写出（实测 out_f.bin 16884B 正常落盘），仅 `.hex` 产物缺失 + 进程以异常退出。
- 修复建议（后续单独提交）：`check = (~sum(bytes.fromhex(ext[j:j+2])[0] for j in …) + 1) & 0xFF` 或改用 `int(ext[j:j+2], 16)` 累加 / `binascii.unhexlify`。

### N2【轻微·风格】新增闸用 `sys.exit(1)`，同文件其余错误分支用 `return 1`
- `merge_prod_bin.py:116` `sys.exit(1)` vs `:95/:99/:101`（boot 不存在/app 不存在/尺寸超限）均 `return 1`。
- CLI 场景 rc 行为一致（实测 rc=1）；差异仅在 main() 被程序化 import 调用时表现为抛 SystemExit 而非返回值。不影响任务书验收口径，属风格不一致，可留待后续统一。

### N3【轻微·语义】无效/不可读 `--key` 走 FAIL 而非 skipped
- 实测（TEST 6）：随机垃圾 `.pem`（不含 PRIVATE 子串）→ 原文直写 → `openssl dgst -verify` exit 1 → `ecdsa: FAIL openssl exit 1` → `RESULT: FAIL` rc=1。
- 判定：方向是 **fail-closed**（验签无法证实即不放行），对验证工具属可接受的保守语义；仅提示文案上"key 不可读"与"签名不匹配"共用同一 exit code，未来可细化（如 dgst stderr 捕获分类）。不影响正常 key 路径。

### N4【信息】`b"PRIVATE" in key_raw` 子串启发式 + 异常路径临时文件
- 启发式：理论上公钥 PEM 的 base64 正文可能偶然含 "PRIVATE" 字符串（概率 ~1e-11）；即便命中，`openssl pkey -pubout` 对公钥输入会失败 → `rc_pub!=0` → 兜底原文直写 → 行为仍正确（优雅降级）。实测仓内 `docs/keys/public.pem` 含 "PRIVATE" 次数 = 0。
- 临时文件：异常若发生在 NamedTemporaryFile 创建之后、`:145-146` unlink 之前，3 个临时文件将残留 —— 旧版既有模式，本 commit 未恶化；实测 6 次运行零泄漏。

---

## 8. 测试环境与方法

- 宿主：WSL2 Linux 6.6.87.2，Python 3.12.3，OpenSSL 3.0.13（与 coder 自报一致）
- 镜像构造（全部在 /tmp/d4_test，仓库零写入）：header 256B（magic="XATO"@0x00、len@0x04、crc32(firmware)@0x08、签名 64B@0x0C、reserved@0x4C、ts@0x5C）+ firmware 1024B（reset vector @firmware+4 = 0x08004104 ∈ App 窗口）；签名 = `openssl dgst -sha256 -sign docs/keys/private.pem`，DER 手工解析回 R||S 各 32B
- 用例矩阵：合法镜像×{public.pem, private.pem, 无 key, 垃圾 key}、篡改固件×public.pem、reserved 非零×public.pem、merge×{非XATO默认/非XATO+force/XATO默认/XATO+force/短app/--hex}、字节级布局断言、/tmp 泄漏计数
- 静态：git diff 双向比对（diff hunk、常量行、bin_to_ihex 函数体）、全仓 grep（pem_to_sec1/hashlib/PRIVATE）、py_compile、pyflakes 3.4.0

## 9. 结论

```json
{
  "summary": "commit 681a4d1 的 4 项 D4 修复（M1①公钥PEM直写+PRIVATE兜底、M1②单次SHA256、R1 @0x4C非零WARNING、R2 XATO硬闸+--force）全部核实落地且行为正确：合成签名镜像实测 public.pem 与 private.pem 双路径 ecdsa:OK/PASS，篡改镜像 FAIL rc=1 证明验签真实生效；merge 硬闸默认 rc=1 且不写产物、--force 降级 WARNING rc=0、合法路径零扰动、合并产物字节级断言通过；常量/偏移零改动，py_compile+pyflakes clean，无新增 import，未碰固件与其它脚本。附 1 项范围外既有缺陷（--hex bin_to_ihex TypeError，旧版 7af8bcf 同样崩溃、函数体未被本 commit 触碰，非回归）与 3 项轻微/信息级备注，均不阻塞。工作树 review_reports/*.md 的 10 处未暂存删除确认与本 commit 无关。",
  "verdict": "PASS_WITH_NOTES",
  "confidence": 92,
  "issues": [
    {"id": "N1", "severity": "low", "blocking": false, "in_scope": false, "file": "python_tools/1.packaging script/merge_prod_bin.py", "line": "52,61", "desc": "【既有缺陷·范围外】--hex 路径 bin_to_ihex 中 sum(bytes.fromhex(...)…) 抛 TypeError:int+bytes；bin 合并在此之前已完成写出，仅 .hex 缺失+异常退出。函数体与 7af8bcf 完全一致、旧版实跑同样崩溃，非 681a4d1 引入", "fix": "后续单独提交：以 int(hex,16) 或 bytes.fromhex(..)[0] 取整累加修复校验和计算"},
    {"id": "N2", "severity": "info", "blocking": false, "in_scope": true, "file": "python_tools/1.packaging script/merge_prod_bin.py", "line": "116", "desc": "新增 XATO 硬闸用 sys.exit(1)，同文件其它错误分支用 return 1；CLI 下 rc 行为一致（实测 rc=1），仅程序化调用语义不同", "fix": "可选：统一为 return 1 风格，与 :95/:99/:101 一致"},
    {"id": "N3", "severity": "info", "blocking": false, "in_scope": true, "file": "python_tools/1.packaging script/verify_image.py", "line": "142-149", "desc": "无效/不可读 --key 时 openssl dgst exit 1 → ecdsa:FAIL（fail-closed，实测确认）；key 不可读与签名不匹配共用同一退出码，文案不可区分", "fix": "可选：捕获 dgst stderr 区分 key 读取失败与验签失败"},
    {"id": "N4", "severity": "info", "blocking": false, "in_scope": true, "file": "python_tools/1.packaging script/verify_image.py", "line": "129-139,145-146", "desc": "b'PRIVATE' 子串启发式理论可误判公钥 base64 正文（rc_pub!=0 兜底可优雅降级，仓内 public.pem 实测 0 次命中）；异常介于临时文件创建与 unlink 之间时残留 3 个临时文件（旧版既有模式，实测 6 运行零泄漏）", "fix": "可选：try/finally 包裹 unlink；key 类型判定改用 BEGIN 行匹配"}
  ]
}
```

**放行意见**：M1/R1/R2 修复本身**可放行**——验签路径行为级验证通过（正/负向对照齐全），硬闸与 WARNING 语义与任务书一致，改动范围零越权。N1（--hex）建议作为独立小提交跟进修复，不构成对本 commit 的回退理由。
