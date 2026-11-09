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

数值是历史实验的配对中位数，基线不同，不能相加。方法、17 项分数、并发与测量限制见 [实验记录](../quickjs-optimization.md)。此目录是归档时的审查快照：site IC 在首轮测试后补了非 inspector 模式的 opcode 数量静态断言，不能将该快照的源码哈希宣称为首轮计分源码哈希。测试时关闭 PGO、inspector 和诊断计数；这些原型不代表已完成跨平台验证。

归档验证：前五个 variant、紧凑 IC 和共享对象布局修正版的输出与本机保存的已测速候选源码逐字相同；首版 site IC 和 object-layout 的差异仅为首测后补充的非 inspector 私有 opcode 区间静态断言。`retained-ef801eaa --format patch` 与 ef801eaa 当时的正式补丁逐字相同，`object-layout-shared --format patch` 与本次共享对象布局的正式补丁逐字相同。独立临时目录验证了应用结果和拒绝覆盖已有输出。这些是源码重建检查，没有用重建工具重新生成历史性能分数。
