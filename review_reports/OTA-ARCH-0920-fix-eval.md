# OTA-ARCH-0920-FIX-EVAL 收尾批抽查报告（evaluator）

- 基线：8bbe8d8（735fd30..8bbe8d8，7 文件 +19/-230）；HEAD 核实一致；只读抽查
- **verdict: PASS_WITH_NOTES** | blocking=[] | confidence=90

## 抽查结论

1. **N1 读回校验 PASS**：ota_trigger.c 新增读回循环与 boot_metadata.c 侧**逐字同构**（`for(i=0;i<words;i++){if(*(volatile uint32_t*)(addr+i*4)!=src[i]){flash_lock();__enable_irq();return -1;}}` 双侧一致）；失败路径 flash_lock/__enable_irq/return -1 齐全；编译面干净（src/words/i 均为函数头既有局部变量 :3-5，无新符号）；插入位置=编程循环后/flash_lock 前（写后立即验、同一 IRQ-off 窗口）；备区校验失败即中止 save→primary 未触碰→另一副本不破坏 ✓
2. **N2 删除安全性 PASS**：两 uvprojx 对被删文件 FilePath 引用=0（grep -c 双侧 0）；mdk_user/Src 注册版在位（Files 列表在+ls-tree=1）；被删文件树中=0。**NOTE**：IncludePath 仍含 `..\libraries\cmsis\device_support`（清单预期"无指向"与实况不符，但非缺陷）——Keil 仅编译 Files 列表条目，IncludePath 只做头解析且该目录下 at32f422_426.h 等正是工程所需必须保留；目录内已无同名 .c，无误编译路径 ✓
3. **N4 口径 PASS**：core-review.md:66 已修正为 A=3/D=93/R=89/M=15=200（与 evaluator @735fd30 实测一致）；读回表述改为"双侧均含（boot 原有/app N1 补齐）"与改后代码事实一致 ✓
4. **N8 卫生 PASS**：check-ignore 实测 `**/Objects/`（.gitignore:80）对两工程 Objects/x.bin 双命中 ✓；git ls-files 构建产物扫描=空（Objects/bootloader.bin 已删）✓；旧名 qi_wireless.bin 代码/文档引用=0（唯一命中为 evaluator 复审报告自身 N8 历史记录行，非代码引用）✓
5. **范围 PASS**：7 文件全部对应 N1/N2/N4/N8（.gitignore/README/docs11/Objects产物删/冗余.c删/ota_trigger.c/core-review.md），无越权 ✓

## Notes
- IncludePath 检查条目与 Keil 编译机制不符（头解析路径≠编译入口），删除安全性不受影响；后续清单可修订该判据。
- N1 读回循环静态编译面干净；实际编译仍受 N5 前置约束（WSL 无 MDK，双工程 Rebuild 时一并验证）。
- 本批 docs/README 旧名修订属 N8 卫生范畴，与 docs 批口径一致。

*—— 评估员已完成抽查 | 只读铁律：业务代码零触碰*
