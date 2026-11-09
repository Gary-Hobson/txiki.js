# QuickJS 性能实验

当前保留版本 `fa509604` 的独立六轮累计确认：相对原始控制版总分中位 **+9.25%**，6/6 胜，17 项中位为16项提升、1项持平。Espree **+54.30%**、Acorn **+25.34%**、Esprima **+17.15%**、TypeScript **+10.83%**。配置为 Release + ThinLTO、无 PGO/inspector。完整三方结果见 [逐项记录](quickjs-optimization.md)；这些结果不是历史增量的相加。

- [逐项方法和结果](quickjs-optimization.md)：包括保留与舍弃的候选，以及每项的 17 个子测试。
- [架构分析](quickjs-architecture-analysis.md)：引擎成本、属性布局身份、所有权值流与对象构造路线。
- [JIT / Java 优化分析](quickjs-jit-inspired-analysis.md)：跨平台借鉴与已验证原型的边界。
- [整数区域执行实验](quickjs-int-region-analysis.md)：SSA/DAG 原型通过正确性验证，但 WTB 主解释器分派覆盖过低，按覆盖门槛停止；[退役后频率与结构分析](quickjs-region-coverage-analysis.md)进一步排除了三个早退数字解析位点存在大量遗漏机会的假设。
- [正则根搜索裁剪](quickjs-regexp-analysis.md)：对必经起始锚点排除不可能的后续起点，六对总分中位 +3.62%，Espree +47.41%、Acorn +16.14%；增量对照为共享对象布局版。
- [架构实验源码快照](experiments/README.md)：重建已保留和已舍弃的主要候选，无需依赖本机构建目录。
- [属性访问缓存分析](quickjs-site-ic-analysis.md)与[对象构造分析](quickjs-object-construction-analysis.md)：具体实现边界、实测成本及修正验证状态。
- [字符串内建调用分析](quickjs-intrinsic-analysis.md)：调用框架、身份验证及未通过性能门槛的原型。
- `results/`：原始分数、采样和内存数据。分数是吞吐量，越高越好。

## 复测现有二进制

使用 Python 3.11 或更新版本，以及 Web Tooling Benchmark 的 classic script `dist/cli.js`。本轮使用 QuickJS-ng fork 的 `a0b6a350`（0.5.3），包含 17 项测试。先分别构建控制版和候选版：Release、ThinLTO、关闭 PGO 与 inspector；检查各自的编译命令和 CMakeCache，不能仅根据目录名认定构建配置。

```sh
python3 benchmarks/run-web-tooling.py \
  --binary baseline=/absolute/path/to/baseline/tjs \
  --binary candidate=/absolute/path/to/candidate/tjs \
  --suite /absolute/path/to/web-tooling-benchmark/dist/cli.js \
  --rounds 6 \
  --output /absolute/path/to/new-results
```

工具对每个二进制完整预热一次，然后交替执行六轮；可增加第三个 `--binary` 做三方比较，六轮覆盖全部排列。默认每次执行超时 120 秒。它通过 `tjs eval` 读取并执行 bundle，最后显式 `tjs.exit(0)`，避免残留句柄阻止退出。每项吞吐量由 benchmark 自身测量，进程墙钟时间另行记录。

输出目录必须不存在，避免覆盖已有证据。每次运行保存完整日志并更新 `comparison.json`，同时记录二进制及 bundle 的 SHA-256、平台、命令和预热方法；`summary.json` 为相对第一个二进制的逐项配对变化中位数与胜出次数。中断或失败时保留此前结果和当前日志。该工具用于新复测，历史数据来自当时保存在本机构建目录的实验脚本，未通过新工具重新生成。

构建目录中的原型变换与诊断二进制不属于产品代码。正式保留实现见 `patches/quickjs-performance.patch`，由 CMake 在构建副本中应用；QuickJS 子模块保持原始提交。硬件计数器未提供有效数据，因此当前不能将吞吐变化直接宣称为实测 cache/branch 命中率改善。
