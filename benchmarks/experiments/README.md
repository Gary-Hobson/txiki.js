# 可重建的架构实验快照

这些变换保存已经完成的实验假设，包含未被采纳的代码。产品只使用仓库的 `patches/quickjs-performance.patch`，不会导入本目录。

输入必须为 QuickJS 子模块 `7b5f588e0f9b643e2996f291bfc861abeaceddc2` 的原始 `quickjs.c`。变换检查源码锚点，拒绝重复应用；不能据此认为支持其他引擎版本。Python 运行不编译、不测速、不改动输入源码，输出文件必须尚不存在。

```sh
python3 benchmarks/experiments/replay.py \
  --source deps/quickjs/quickjs.c \
  --variant object-layout-shared \
  --format patch \
  --output /tmp/quickjs-object-layout.patch
```

省略 `--format patch` 即输出完整候选 C 文件。构建应使用独立 QuickJS 源码副本、相同的其他依赖和编译选项，再用 [WTB 执行工具](../README.md) 比较；不要将多个变换任意叠加，私有 opcode 可能冲突。

| variant | 对应实验 |
|---|---|
| `hint-fusion` | c4cc952f 保留的属性槽提示与局部字段融合 |
| `retained-ef801eaa` | 再加入 ef801eaa 的比较与条件分支融合 |
| `borrow-predicate` | 首版借用谓词，独立六对 -7.13%，舍弃 |
| `borrow-quickening` | 入口特化修正版，独立六对 +0.59% |
| `hint-fusion-quickening` | 修正版叠加已有优化，六对 -1.00%，舍弃 |
| `site-read-ic` | 第一版按访问点缓存，增量六对 -1.28%，舍弃 |
| `site-read-ic-packed` | 紧凑布局修正版，增量六对 -0.86%，舍弃 |
| `object-layout` | 第一版对象初始化预规划，相对 ef801eaa 六对 -0.14%，espree -5.98%，舍弃 |
| `object-layout-shared` | 共享最终形状的修正版，相对 ef801eaa 六对 +1.41%，已保留 |
| `object-string-intrinsic` | 对共享对象布局加入 charCodeAt 纯值快路，六对 -0.28%，舍弃 |
| `object-int-region` | 事务式整数DAG区域，净减少约0.079%主VM分派，按覆盖门槛拒绝；未做计分 |

数值是历史实验的配对中位数，基线不同，不能相加。方法、17 项分数、并发与测量限制见 [实验记录](../quickjs-optimization.md)。此目录是归档时的审查快照：site IC 在首轮测试后补了非 inspector 模式的 opcode 数量静态断言，不能将该快照的源码哈希宣称为首轮计分源码哈希。测试时关闭 PGO、inspector 和诊断计数；这些原型不代表已完成跨平台验证。

归档验证：前五个 variant、紧凑 IC 和共享对象布局修正版的输出与本机保存的已测速候选源码逐字相同；首版 site IC 和 object-layout 的差异仅为首测后补充的非 inspector 私有 opcode 区间静态断言。`retained-ef801eaa --format patch` 与 ef801eaa 当时的正式补丁逐字相同，`object-layout-shared --format patch` 与52becd9e时共享对象布局的正式补丁逐字相同（不包含之后新增的libregexp.c补丁）。独立临时目录验证了应用结果和拒绝覆盖已有输出。这些是源码重建检查，没有用重建工具重新生成历史性能分数。

字符串 intrinsic 也验证了重建源码与计分候选逐字一致，专项 JS/C fixture 一并归档。`native-call-profile-transform.py` 是独立频率诊断；该插桩版本的吞吐分数不能用于优化评价。原生 fixture 的归档副本经过 C 格式化，程序字符串保持不变。

整数区域的transform、JS专项和独立差分fixture按原文归档；native C fixture在通过测试后仅做clang-format格式化，执行副本与格式化归档的哈希分别记录，未在独占计时窗口追加格式化后的运行。`object-int-region`重建源码与实际验证候选逐字一致，SHA-256为 `233e7987e9001d28d6adaa713901031848e23569556b5d90766516dbcba56160`。完整输入与fixture哈希见 [int-region-replay-evidence.json](int-region-replay-evidence.json)，正确性、分配失败和同run分派计数见 [诊断数据](../results/int-region-diagnostics.json)，方法及拒绝边界见 [分析报告](../quickjs-int-region-analysis.md)。它通过语义验证，但因覆盖低而未运行六对，不能将其记成实测吞吐退化。

`field-chain`重建连续自有字段借用循环：六对相对fa509604中位−3.23%，拒绝。此variant仅生成quickjs.c，计分链接的libregexp.c来自已保留的正则裁剪版本，参见[完整记录](../quickjs-field-chain-analysis.md)。

`field-chain-outline`将续读循环移到独立helper，三方六轮相对fa509604中位−9.79%，拒绝；源代码重建与测试候选逐字一致。

字符串生命周期、charCodeAt body-only变换及JS/native差分fixture保存为 `string-lifecycle-*`。这些独立脚本以正则裁剪构建副本为受哈希校验的输入，重建已计分源码；不属于上表 `replay.py` 的variant。具体运行命令、诊断与吞吐的区分见[生命周期报告](../quickjs-string-lifecycle-analysis.md)。正式快路保留，两次独立六轮 +0.54% / +0.80%；未增加Map哈希缓存。

`producer-region-transform.py` 的纯 `transform(source)` 接口以body-only quickjs.c为输入，重建调用点与后继区域融合候选；`producer-region-*` JS/native专项覆盖检查点、重入、bytecode和失败回退。六对相对body-only −1.21%、0/6胜，拒绝；[重放指纹](../results/producer-region-replay-evidence.json)和[完整方法](../quickjs-producer-region-analysis.md)记录未完成集成门槛，勿误用为产品代码。

`producer-region-runtime-gate-transform.py` 只给失败候选增加准备阶段的实验开关，用同一binary区分执行快路与框架成本。开启/关闭六对+0.41%，但开启/body−1.07%、关闭/body−1.47%，不是保留优化。输入、环境、等量plan验证及三方六排列见[独立报告](../quickjs-producer-region-runtime-gate-analysis.md)。
