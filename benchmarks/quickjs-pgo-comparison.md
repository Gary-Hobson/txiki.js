# 当前保留版本的 PGO 对比

本次仅执行用户指定的PGO测试，没有恢复持续优化目标或额度定时任务。生产源码没有变化；计分代码对应已保留的charCodeAt版本（ba423c89源码），仓库HEAD为f0899e13。

## 结果

Web Tooling Benchmark综合分数是17项吞吐量的几何平均（runs/s，越高越好）。下表再对六轮综合分数计算均值和中位数；提升为同轮配对比值的中位数。

| 版本 | 六轮平均分 | 六轮中位分 |
|---|---:|---:|
| 旧PGO二进制，本次重测 | 3.975 | 3.975 |
| 当前保留代码 + PGO | 4.327 | 4.330 |
| 当前保留代码，仅ThinLTO | 3.735 | 3.745 |

新PGO/旧PGO：配对中位 **+8.92%**，6/6胜。新PGO/当前ThinLTO：**+15.58%**，6/6胜。相对旧PGO，16项配对中位为正，chai为−0.91%。

旧PGO历史记录均值3.86（当时六分3.83、3.85、3.92、3.93、3.86、3.77）。本次仍用原二进制重新测得均值3.975；跨时间的3.86仅作历史参照，主要比较采用同一测试窗口。旧二进制的完整历史源码快照未建立，因此不把全部新旧差值归因于某一个源码补丁。

| 子项 | 新PGO / 旧PGO | 新PGO / 当前ThinLTO |
|---|---:|---:|
| acorn | +27.11% | +13.71% |
| babel | +6.31% | +12.18% |
| babel-minify | +5.13% | +11.35% |
| babylon | +7.64% | +10.83% |
| buble | +8.89% | +14.94% |
| chai | -0.91% | +36.34% |
| espree | +64.71% | +13.33% |
| esprima | +17.67% | +12.42% |
| jshint | +3.15% | +13.64% |
| lebab | +2.59% | +20.87% |
| postcss | +5.38% | +17.09% |
| prepack | +2.87% | +9.00% |
| prettier | +1.30% | +17.11% |
| source-map | +6.72% | +22.17% |
| terser | +1.30% | +19.22% |
| typescript | +8.13% | +8.20% |
| uglify-js | +1.20% | +19.62% |
| Geometric mean | +8.92% | +15.58% |

## 方法与profile匹配

Apple M4、macOS 15.6.1、Apple clang 17；Release + ThinLTO，inspector OFF。固定WTB bundle SHA-256 a7a227840538deb2d44dcadbf3da6b12187b29961f2e134db2269b785f7d9931。旧PGO二进制与profile均保留原样；新版本使用三个完整WTB训练进程生成IR profile，再通过llvm-profdata合并。训练和计分使用同一工作负载，因此结论针对该WTB工作负载，不证明其他应用同样获益。

插桩使用C/CXX的-fprofile-generate，最终编译使用-fprofile-use=<新tjs.profdata>。初次在另一个use目录构建，因CMake复制QuickJS源码导致绝对路径不同，静态函数profile没有匹配：额外LLVM missing-function诊断显示quickjs缺1081条、libregexp缺38条；普通编译warning未暴露问题。该试构建不参与计分。

最终先保存插桩二进制及其配置，然后在同一个generate构建目录重配profile-use并全量重建，保持真实源码路径一致。QuickJS缺失条目降至104，JS_CallInternal和js_string_charCodeAt不再缺失；libregexp降至0，没有profile hash mismatch警告。剩余条目意味着并非所有函数都在训练中执行过。最终quickjs.c和libregexp.c哈希与正式保留版本一致，六项优化回归通过。

当前有效PGO二进制：`build/lto-benchmark/pgo-retained/final/tjs`（同路径重建产物的副本）；不要把未匹配的`pgo-retained/use/tjs`用于结果复现。三方各完整预热一次，六轮覆盖三个版本的全部排列，共18次计分；窗口内本任务不运行其他构建或性能测试。

主代理另行核对四份源码/profile哈希、三份计分二进制和bundle哈希，以及最终实际compile_commands中的ThinLTO/profile-use；独立重算全部逐项配对结果。当前ThinLTO第五轮分数为3.67，其余3.73–3.76；完整保留该轮，采用配对中位数汇总。

## 原始证据

- [18次原始分数](results/retained-pgo-triad.json)
- [配置、训练、profile与二进制指纹](results/retained-pgo-metadata.json)
- [逐项统计及历史样本](results/retained-pgo-summary.json)

本轮仅新增报告与结果文件，未修改正式引擎，未恢复暂停任务。
