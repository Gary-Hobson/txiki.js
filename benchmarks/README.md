# QuickJS 性能实验

- [逐项方法和结果](quickjs-optimization.md)：包括保留与舍弃的候选，以及每项的 17 个子测试。
- [架构分析](quickjs-architecture-analysis.md)：引擎成本、属性布局身份、所有权值流与对象构造路线。
- [JIT / Java 优化分析](quickjs-jit-inspired-analysis.md)：跨平台借鉴与已验证原型的边界。
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
