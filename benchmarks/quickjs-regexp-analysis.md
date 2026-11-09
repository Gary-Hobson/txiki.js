# QuickJS 正则根搜索裁剪实验

日期：2026-11-10。基线为正式 `d2b800da` 所对应的 `build/lto-benchmark/integrated-object-layout/tjs`（engine patch 实现 `52becd9e`），Apple M4 / macOS arm64，Release + ThinLTO，PGO 与 inspector OFF。这里只记录正则子系统的独立实验与验证；主代理已根据六对结果决定保留，负责正式 patch/CMake 集成。`deps/quickjs` gitlink 不变。所有收益均相对已经保留对象布局等优化的 `52becd9e` engine 基线，是增量收益，不是相对原始引擎的总收益。

## 从可核验计数选择优化对象

旧的 10 秒 sample 中 `lre_exec` self 为 9.35%，不代表完整 WTB 17 项的时间占比。本次没有用它预测倍数，而是在当前基线 `libregexp.c` 加只用于诊断的 opcode/root-search 计数，完整运行 WTB 17 项。插桩会改变吞吐、自动迭代次数与布局，所以计数用于定位重复工作，诊断分数不能作为性能结果。

诊断记录：[regexp-diagnostic.log](results/regexp-diagnostic.log)。可重放的插桩 patch：[regexp-diagnostic.patch](experiments/regexp-diagnostic.patch)。完成全部 17 项、进程 exit 0，统计为：

| 计数 | 次数 |
|---|---:|
| `lre_exec` 调用 | 4,656,754 |
| 正则 VM opcode 执行 | 1,053,470,705 |
| 非 sticky 根搜索起点尝试 | 183,672,821 |
| 跳过无条件 capture-start 后首指令为 `line_start`（非 multiline `^`）的尝试 | 172,418,818 |
| 首指令为 `char` 的尝试 | 4,470,970 |
| 上述 `char` 的首字符拒绝 | 4,381,811 |

首指令为必经 `^` 占根搜索尝试 **93.8728%**；字面首字符拒绝只占 **2.3857%**。这些是这次插桩运行的执行次数，既不是耗时百分比，也不是各子项平均值。

源码原因：`lre_compile` 对所有非 sticky 模式生成相同的 `split_goto_first(+6) → any → goto(-11)` 前导。根 body 首部是 `save_start(0)`，捕获组还可能继续生成 `save_start`。即使 body 必须先执行非 multiline 的 `^`，起点 0 失败后，原实现仍把起点逐字符推进到末尾，每次重建根回溯帧、保存捕获、检查必然失败的锚点再撤销。这是被实际执行计数证实的结构性重复工作。

## 候选 A：必经起始锚点裁剪

唯一送入正式六对计时的候选为 **anchor-only**，转换脚本是 [regexp-anchor-transform.py](experiments/regexp-anchor-transform.py)，不是早期 `regexp-transform.py` 的 literal/anchor 混合原型。

每次 `lre_exec` 只识别现有原始字节码中的固定根前导，跳过紧邻的无条件 `save_start`，若随后恰为 `REOP_line_start`，在当前执行上下文记下根 split 的 PC。执行该 split 时，如果归一化后的输入指针已不等于输入开头，直接进入原 `no_match` 路径；否则完全照原 VM 执行。

关键边界：

- 不改变编译结果、opcode 编号、头格式、序列化或公开字节码兼容性；每次执行仅多一个上下文指针，不引入缓存生命周期或分配。
- 初次起点 0 的根 frame 完整保留。body 失败先由原机制恢复所有 captures，随后再次进入根 split 才剪枝。原生 API 在失败时的 capture 状态也保持一致。
- 判断使用已经按 Unicode surrogate 规则归一化的 `cptr`，不以 `cindex == 0` 代替。
- 不跨越 split、lookaround、可选/重复或其它指令；`/^a|b/`、`/(^)?a/`、`/(?=^a)a/` 等走原 VM；`line_start_m`（`m`）也不准入。sticky 没有此根前导，保持原 VM。
- `^` 后的 captures、backrefs、lookaround、Unicode u/v、字符类和回溯仍由原解释器执行。
- 对可证明没有后续候选的锚定路径，不再扫描余下输入，因此不会保留原来无意义扫描所产生的 timeout 回调次数。它没有新增长循环；未准入的长扫描、回溯仍保持原有 poll 行为。这与“新增扫描循环却不 poll”不同。
- 实现为可移植 C；本次只实测 macOS arm64，未实测 Windows、Linux、32-bit 或其它架构。

源码及二进制 SHA-256 锁定在 [regexp-anchor-evidence.json](results/regexp-anchor-evidence.json)。送入计时的纯净 candidate `libregexp.c` SHA-256 为 `24ea589d535ab3a58627b64ba795647aa62eb94f60ee9bcd2eaf9d496c8d8dde`。

## 正确性与中断验证

[regexp-correctness.c](experiments/regexp-correctness.c) 同时链接基线与候选执行器，使用同一份编译字节码（候选未修改编译函数）、同一输入对比返回值和全部公开 capture 指针，失败返回也比较。在 `-O1 -fsanitize=address,undefined -fno-omit-frame-pointer` 构建下完成 **2,423,512 个矩阵差分 cases**，exit 0，无 sanitizer 报告。

这些 cases 来自 47 个固定模式、14 组 flags、每组 100 个确定性输入（0–39 code units）、8/16-bit 存储、所有 0…length 起点的组合，以及长输入专项；不是 242 万个独立模式。不合法/不支持的模式与 flags 组合由编译器拒绝后跳过。模式覆盖捕获锚点、可选锚点、多分支、lookahead/lookbehind、backref、空匹配、Unicode astral/lone surrogate、u/v flags、Unicode 属性和集合等。随机输入字母表含换行、Latin-1、BMP、孤立代理项和代理对片段，固定 seed `0x573812`。

200,000 字节长输入另外验证 literal 与其它 fallback，在第 1/第 3 次 timeout callback 返回中断时，两执行器的返回值、captures 和实际 callback 次数一致；锚定快速失败专项比较返回值与 captures，明确不要求原本已消除的搜索 callback 次数相同。

Terra 执行的 JS 层专项在基线及纯净候选均通过，覆盖 `g/y/m`、lastIndex、Unicode、捕获、match/search 和 replace callback。正式单功能回归 [test-regexp-anchor.js](../tests/test-regexp-anchor.js) 也在这两个二进制均通过，另覆盖 matchAll/split、search 保留 lastIndex、u/v 的低代理起点与 lone surrogate、indices、backrefs、lookaround、零长度、compile/evalBytecode 后的正则语义。

归档 C fixture 按仓库 clang-format 格式化；原始未格式化 fixture 仍在 build 目录且原 SHA-256 保留于 evidence。格式化副本由归档 `regexp-test-build.py` 直接编译（明确输入 `benchmarks/experiments/regexp-correctness.c`，不再误用 build 中原稿），再次完成相同 **2,423,512 cases**，ASAN+UBSAN exit 0，见 [格式化复验记录](results/regexp-anchor-native-correctness-formatted.log)。正式源码集成后的完整 suite、编译来源 hash 与 configure 重入验证由主代理统一完成。

构建/验证入口（在仓库根运行）：

```sh
python3 benchmarks/experiments/regexp-anchor-transform.py
python3 benchmarks/experiments/regexp-build.py regexp-anchor
python3 benchmarks/experiments/regexp-test-build.py regexp-anchor
build/lto-benchmark/regexp-analysis/regexp-anchor/regexp-correctness
```

这些脚本依赖已存在且未改写的 `integrated-object-layout` 基线构建目录。变体只替换其 libqjs archive 中的 `libregexp.c.o` 并按原 ThinLTO 链接命令重链；构建输出、日志都放独立 `regexp-analysis` 下。

## 计时与决定

Terra 在主代理安排的 CPU 独占窗口完成 1 次/二进制预热和 **AB/BA 交替六对**，每次完整 WTB 17 项。原始结果：[regexp-anchor.json](results/regexp-anchor.json)，平台/套件/二进制指纹：[regexp-anchor-metadata.json](results/regexp-anchor-metadata.json)。总分配对中位 **+3.6212%**，均值约 **+3.71%**，**6/6 胜**；基线总分 3.58–3.60 runs/s，候选 3.72–3.74 runs/s。分数来自 WTB 输出的两位小数，较小子项变动须考虑输出量化。

| WTB 子项 | 六对配对中位变化 |
|---|---:|
| acorn | +16.14% |
| babel | 0.00% |
| babel-minify | +0.60% |
| babylon | -0.18% |
| buble | +2.63% |
| chai | -0.36% |
| espree | +47.41% |
| esprima | -0.45% |
| jshint | -0.65% |
| lebab | +0.38% |
| postcss | +1.70% |
| prepack | -1.01% |
| prettier | +0.19% |
| source-map | +2.80% |
| terser | +1.09% |
| typescript | +0.27% |
| uglify-js | -0.31% |

主代理依用户允许“大项明显收益、小项小幅回退”的标准决定保留并进行正式集成。结果支持消除已证实的锚定搜索冗余能带来稳定整套增量；不代表所有正则更快，也不代表 JIT、跨平台倍率或未实测 literal scanner 的收益。

## 未送入计时的字面首字符原型

最初假设是对固定根前导后必经 `char/char32` 做起点扫描，省掉拒绝位置的 frame/capture/dispatch。构建了实验源码，设计按 Unicode `GET_CHAR` 前进，每个拒绝位置保留原来的两次 poll、EOF 保留一次 poll；不接受 ignore-case、字符类、可选分支或 lookaround 前导。完整计数揭示必经锚点问题远大于字面首字符筛选，故立即改为单独验证 anchor-only。字面首字符候选没有正式差分/计时结果，未保留；不能把它的潜在收益归到本次候选。

## 正式集成验证

独立构建目录 `build/lto-benchmark/integrated-regexp-anchor` 的Release + ThinLTO、无PGO、inspector OFF→ON→OFF构建完成。quickjs.c仍与52becd9e一致；libregexp.c与计分候选逐字一致，第二次configure后也未被普通复制覆盖。五项专项回归在三次配置均通过，ON inspector测试通过。OFF完整套件506项通过、1项inspector跳过、1项原始基线亦失败的TCP中止测试；详见 [集成验证记录](results/regexp-anchor-integration.json)。
