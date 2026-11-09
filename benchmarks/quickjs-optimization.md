# QuickJS优化实验记录

日期为2026-11-10，Asia/Taipei。每项完成后更新。

## 方法

Apple M4，macOS 15.6.1，Apple clang 17，arm64。Release + ThinLTO，关闭PGO及inspector。QuickJS基线7b5f588e0f9b643e2996f291bfc861abeaceddc2，txiki包含UTF-8优化d60babc2。

测试Web Tooling Benchmark 0.5.3，QuickJS-ng fork a0b6a350，共17子项。通过tjs eval、TextDecoder和global eval加载classic script，完成后显式退出。基线和候选交替执行，逐项独立修改，串行测速。变化为配对比值的中位数，正数表示吞吐提高。早期实验初筛4对，有明显收益时追加复测，并检查每个子项退化；atom-index后四对、length-fast和后续集成测量与array-inline、map-numeric-hash、load-field-fusion阶段使用并行执行器；后三项各6对，与先前串行结果分开解读。atom-index及之后的新候选正式计时前，基线和候选各完整预热一套；此前实验无单独完整预热。

构建配置：`cmake -B build/lto-benchmark/source-opt -DCMAKE_BUILD_TYPE=Release -DBUILD_WITH_LTO=ON -DBUILD_WITH_INSPECTOR=OFF`，采用ThinLTO；CMAKE_C_FLAGS/CXX_FLAGS未加任何`-fprofile-*`选项。实验从compile_commands重编修改后的quickjs.c，替换libqjs.a中的对象并重新链接。正式测量命令：`TJS_HOME=<隔离目录> WEB_TOOLING_BUNDLE=<dist/cli.js> <tjs> eval "globalThis.eval(new TextDecoder().decode(await tjs.readFile(tjs.env.WEB_TOOLING_BUNDLE))); tjs.exit(0);"`。脚本与编译日志保存在本机build/lto-benchmark/quickjs-hotpaths目录，该构建目录不纳入Git。CPU Counters默认模板未导出可用命中率，缓存/分支改善机制尚不能用硬件计数器确认。

基线独立采样于2026-11-10 02:17:32 +0800开始，macOS sample持续10秒、1毫秒采样周期，运行不计入分数。采样从启动约1秒后开始，只覆盖部分子项，并非整套占比或dispatch占比。排除等待中的worker，主线程8522样本：JS_CallInternal栈顶5313（62.34%），lre_exec 797（9.35%），JS_RunGC自身14（0.16%）。原始调用栈见 [baseline-sample.txt](results/baseline-sample.txt)。

独立诊断构建统计运行期opcode和动态相邻对，跨调用也可能相邻；计数非耗时或硬件周期，插桩结果不计分。详见 [前20摘要](results/opcode-profile.md) 与 [完整JSON](results/opcode-profile.json)。

集成验证：OFF构建及属性回归通过。完整套件有4项失败；TCP中止测试基线亦同样失败，三个bundle测试在基线和集成版单独运行均通过。相同构建目录OFF→ON→OFF切换时fingerprint变化，ON构建及inspector测试通过，OFF重建通过。最终hint+fusion组合的完整套件503通过、1跳过、1失败；失败为原始基线同样失败的TCP中止测试，property-cache及load-field-fusion专项回归通过。

## 已提交的UTF-8优化

提交d60babc2，每次按8字节检查并复制ASCII，受输入及输出容量限制。四轮解码吞吐中位数，单位MB/s：ASCII 523.382→16915.875；稀疏Unicode 270.553→581.545；密集Unicode 279.443→274.338。编码测试99/99及新增流式回归通过。Web Tooling四轮均分3.260→3.345，解码在子项计时之外，不能把总分变化归因于解码优化。

## 尺寸查表

65字节表替代尺寸分级分支。

状态：舍弃，收益不稳定或退化。

整体配对中位变化+0.60%，胜出5/8对。

| 轮次 | 基线 | 候选 | 变化 |
|---|---:|---:|---:|
| 1 | 3.05 | 2.78 | -8.85% |
| 2 | 2.99 | 3.16 | +5.69% |
| 3 | 3.07 | 3.01 | -1.95% |
| 4 | 3.11 | 3.26 | +4.82% |
| 5 | 3.32 | 3.34 | +0.60% |
| 6 | 3.30 | 3.37 | +2.12% |
| 7 | 3.36 | 3.33 | -0.89% |
| 8 | 3.36 | 3.38 | +0.60% |

| 子项 | 配对中位变化 |
|---|---:|
| acorn | +0.36% |
| babel | +0.00% |
| babel-minify | +0.96% |
| babylon | +1.41% |
| buble | +1.50% |
| chai | +4.63% |
| espree | +0.46% |
| esprima | -0.84% |
| jshint | -0.45% |
| lebab | +1.20% |
| postcss | +1.60% |
| prepack | +1.00% |
| prettier | +2.00% |
| source-map | +3.52% |
| terser | +2.14% |
| typescript | +2.87% |
| uglify-js | +0.66% |

完整原始数据见 [benchmarks/results/allocation.json](results/allocation.json)。

## Map比较

同标签整数、对象及Symbol直接比较。

状态：舍弃，收益不稳定或退化。

整体配对中位变化-3.19%，胜出1/4对。

| 轮次 | 基线 | 候选 | 变化 |
|---|---:|---:|---:|
| 1 | 3.20 | 3.13 | -2.19% |
| 2 | 3.36 | 3.15 | -6.25% |
| 3 | 3.24 | 3.35 | +3.40% |
| 4 | 3.34 | 3.20 | -4.19% |

| 子项 | 配对中位变化 |
|---|---:|
| acorn | -0.33% |
| babel | -0.57% |
| babel-minify | +0.01% |
| babylon | -1.01% |
| buble | -0.51% |
| chai | -1.27% |
| espree | -5.03% |
| esprima | -4.26% |
| jshint | -10.11% |
| lebab | -2.66% |
| postcss | -0.73% |
| prepack | -2.38% |
| prettier | +2.67% |
| source-map | -1.47% |
| terser | +0.00% |
| typescript | -3.95% |
| uglify-js | -0.32% |

完整原始数据见 [benchmarks/results/map-keys.json](results/map-keys.json)。

## 小形状查找

最多两个属性槽位直接扫描，省去桶读取。

状态：舍弃，收益不稳定或退化。

整体配对中位变化-0.15%，胜出2/4对。

| 轮次 | 基线 | 候选 | 变化 |
|---|---:|---:|---:|
| 1 | 3.26 | 3.27 | +0.31% |
| 2 | 3.29 | 3.25 | -1.22% |
| 3 | 3.19 | 3.27 | +2.51% |
| 4 | 3.33 | 3.31 | -0.60% |

| 子项 | 配对中位变化 |
|---|---:|
| acorn | -2.40% |
| babel | +0.59% |
| babel-minify | +0.87% |
| babylon | +0.61% |
| buble | -1.92% |
| chai | +0.61% |
| espree | +0.00% |
| esprima | +0.27% |
| jshint | -0.23% |
| lebab | -0.54% |
| postcss | +0.00% |
| prepack | +1.12% |
| prettier | +0.00% |
| source-map | -1.15% |
| terser | +0.00% |
| typescript | +1.00% |
| uglify-js | +0.35% |

完整原始数据见 [benchmarks/results/properties.json](results/properties.json)。

## 分配器内联

强制内联js_arena_malloc，减少调用开销。

状态：舍弃，收益不稳定或退化。

整体配对中位变化+0.30%，胜出2/4对。

| 轮次 | 基线 | 候选 | 变化 |
|---|---:|---:|---:|
| 1 | 3.26 | 3.26 | +0.00% |
| 2 | 3.34 | 3.33 | -0.30% |
| 3 | 3.35 | 3.37 | +0.60% |
| 4 | 3.25 | 3.34 | +2.77% |

| 子项 | 配对中位变化 |
|---|---:|
| acorn | +0.34% |
| babel | +2.33% |
| babel-minify | +0.84% |
| babylon | +0.40% |
| buble | +0.82% |
| chai | +1.34% |
| espree | +0.00% |
| esprima | +0.00% |
| jshint | -2.72% |
| lebab | -0.13% |
| postcss | +0.27% |
| prepack | +1.13% |
| prettier | +3.53% |
| source-map | +1.35% |
| terser | -0.51% |
| typescript | +0.00% |
| uglify-js | +0.82% |

完整原始数据见 [benchmarks/results/inline-arena.json](results/inline-arena.json)。

## 相等比较内联

强制内联js_strict_eq2，允许比较模式专门化。

状态：舍弃，收益不稳定或退化。

整体配对中位变化-0.92%，胜出1/4对。

| 轮次 | 基线 | 候选 | 变化 |
|---|---:|---:|---:|
| 1 | 3.31 | 3.30 | -0.30% |
| 2 | 3.29 | 3.31 | +0.61% |
| 3 | 3.34 | 3.22 | -3.59% |
| 4 | 3.25 | 3.20 | -1.54% |

| 子项 | 配对中位变化 |
|---|---:|
| acorn | -3.10% |
| babel | -0.43% |
| babel-minify | -0.15% |
| babylon | +0.61% |
| buble | +1.34% |
| chai | +1.43% |
| espree | +0.93% |
| esprima | -1.06% |
| jshint | +0.70% |
| lebab | +0.29% |
| postcss | -6.29% |
| prepack | -7.73% |
| prettier | +0.03% |
| source-map | -1.12% |
| terser | +1.04% |
| typescript | +0.50% |
| uglify-js | +0.83% |

完整原始数据见 [benchmarks/results/inline-equality.json](results/inline-equality.json)。

## 原型查询内联

强制内联JS_GetPrototypePrimitive。

状态：舍弃，收益不稳定或退化。

整体配对中位变化+0.29%，胜出2/4对。

| 轮次 | 基线 | 候选 | 变化 |
|---|---:|---:|---:|
| 1 | 3.33 | 3.37 | +1.20% |
| 2 | 3.27 | 3.33 | +1.83% |
| 3 | 3.23 | 3.21 | -0.62% |
| 4 | 3.30 | 3.12 | -5.45% |

| 子项 | 配对中位变化 |
|---|---:|
| acorn | -0.68% |
| babel | -1.02% |
| babel-minify | -3.67% |
| babylon | +0.61% |
| buble | +1.44% |
| chai | +0.68% |
| espree | +0.46% |
| esprima | -1.36% |
| jshint | -0.46% |
| lebab | -0.81% |
| postcss | +0.93% |
| prepack | +1.88% |
| prettier | -2.56% |
| source-map | +0.91% |
| terser | +0.00% |
| typescript | -0.90% |
| uglify-js | -1.83% |

完整原始数据见 [benchmarks/results/inline-prototype.json](results/inline-prototype.json)。

## 字符串哈希展开

四字符多项式展开，保持哈希值一致，缩短乘加依赖链。

状态：舍弃，收益不稳定或退化。

整体配对中位变化-2.10%，胜出0/4对。

| 轮次 | 基线 | 候选 | 变化 |
|---|---:|---:|---:|
| 1 | 3.33 | 3.26 | -2.10% |
| 2 | 3.27 | 3.15 | -3.67% |
| 3 | 3.33 | 3.26 | -2.10% |
| 4 | 3.29 | 3.23 | -1.82% |

| 子项 | 配对中位变化 |
|---|---:|
| acorn | -3.13% |
| babel | -4.16% |
| babel-minify | -1.50% |
| babylon | -1.41% |
| buble | +0.10% |
| chai | -1.04% |
| espree | +0.46% |
| esprima | -2.13% |
| jshint | -2.77% |
| lebab | -4.14% |
| postcss | -1.57% |
| prepack | -2.97% |
| prettier | +0.20% |
| source-map | -1.82% |
| terser | -1.01% |
| typescript | +0.70% |
| uglify-js | -3.14% |

完整原始数据见 [benchmarks/results/hash-blocks.json](results/hash-blocks.json)。

## Map哈希位扩散

在桶掩码前混合高低位，缓解数值及指针低位重复造成的碰撞。

状态：舍弃，收益不稳定或退化。

整体配对中位变化-0.90%，胜出1/4对。

| 轮次 | 基线 | 候选 | 变化 |
|---|---:|---:|---:|
| 1 | 3.24 | 3.11 | -4.01% |
| 2 | 3.29 | 3.34 | +1.52% |
| 3 | 3.29 | 3.27 | -0.61% |
| 4 | 3.34 | 3.30 | -1.20% |

| 子项 | 配对中位变化 |
|---|---:|
| acorn | -1.75% |
| babel | +2.58% |
| babel-minify | +6.45% |
| babylon | -1.60% |
| buble | -0.62% |
| chai | +1.06% |
| espree | +0.93% |
| esprima | -1.34% |
| jshint | -2.43% |
| lebab | -2.27% |
| postcss | -1.04% |
| prepack | +3.78% |
| prettier | +0.98% |
| source-map | +0.34% |
| terser | -2.56% |
| typescript | -1.29% |
| uglify-js | +2.07% |

完整原始数据见 [benchmarks/results/map-hash.json](results/map-hash.json)。

## atom索引缓存

64位JSString填充空间存储atom索引，省去hash链反查；32位保持原实现。

状态：实验完成；当前未纳入提交。

整体配对中位变化+0.75%，胜出7/8对。

| 轮次 | 基线 | 候选 | 变化 |
|---|---:|---:|---:|
| 1 | 3.35 | 3.38 | +0.90% |
| 2 | 3.36 | 3.37 | +0.30% |
| 3 | 3.25 | 3.37 | +3.69% |
| 4 | 3.36 | 3.38 | +0.60% |
| 5 | 3.06 | 3.29 | +7.52% |
| 6 | 3.29 | 3.30 | +0.30% |
| 7 | 3.22 | 3.30 | +2.48% |
| 8 | 3.31 | 3.25 | -1.81% |

| 子项 | 配对中位变化 |
|---|---:|
| acorn | -0.69% |
| babel | +0.57% |
| babel-minify | +1.38% |
| babylon | -0.40% |
| buble | +0.20% |
| chai | +1.42% |
| espree | +0.46% |
| esprima | +0.54% |
| jshint | +0.46% |
| lebab | +0.40% |
| postcss | +0.40% |
| prepack | +2.22% |
| prettier | +0.39% |
| source-map | +0.67% |
| terser | +1.57% |
| typescript | +0.50% |
| uglify-js | +2.52% |

完整原始数据见 [benchmarks/results/atom-index.json](results/atom-index.json)。

## 属性槽位缓存

1024槽整数提示表，每次验证当前shape中的槽位边界与atom，保留flags与原型慢路径。

状态：已集成至提交c4cc952f。

整体配对中位变化+2.70%，胜出7/8对。

| 轮次 | 基线 | 候选 | 变化 |
|---|---:|---:|---:|
| 1 | 3.33 | 3.41 | +2.40% |
| 2 | 3.24 | 3.40 | +4.94% |
| 3 | 3.36 | 3.40 | +1.19% |
| 4 | 3.31 | 3.43 | +3.63% |
| 5 | 3.31 | 3.31 | +0.00% |
| 6 | 3.33 | 3.42 | +2.70% |
| 7 | 3.33 | 3.42 | +2.70% |
| 8 | 3.30 | 3.39 | +2.73% |

| 子项 | 配对中位变化 |
|---|---:|
| acorn | +2.05% |
| babel | +2.29% |
| babel-minify | +3.56% |
| babylon | +3.04% |
| buble | +2.76% |
| chai | +2.76% |
| espree | +1.37% |
| esprima | +3.72% |
| jshint | +0.47% |
| lebab | +1.86% |
| postcss | +3.16% |
| prepack | +3.03% |
| prettier | +0.78% |
| source-map | +1.81% |
| terser | +1.04% |
| typescript | +1.68% |
| uglify-js | +1.98% |

完整原始数据见 [benchmarks/results/property-hint.json](results/property-hint.json)。

## 空arena缓存

每尺寸类别保留一个空arena，减少底层分配与块初始化，增加至多约124KiB预留内存。

状态：舍弃，收益不稳定或退化。

整体配对中位变化-1.07%，胜出1/4对。

| 轮次 | 基线 | 候选 | 变化 |
|---|---:|---:|---:|
| 1 | 3.28 | 3.19 | -2.74% |
| 2 | 3.27 | 3.20 | -2.14% |
| 3 | 3.25 | 3.35 | +3.08% |
| 4 | 3.31 | 3.31 | +0.00% |

| 子项 | 配对中位变化 |
|---|---:|
| acorn | -2.73% |
| babel | +0.43% |
| babel-minify | -4.03% |
| babylon | +1.01% |
| buble | +0.10% |
| chai | +0.39% |
| espree | -0.92% |
| esprima | +1.66% |
| jshint | +0.00% |
| lebab | -4.73% |
| postcss | +0.54% |
| prepack | -1.17% |
| prettier | -7.17% |
| source-map | -3.86% |
| terser | +0.52% |
| typescript | +0.80% |
| uglify-js | +0.84% |

完整原始数据见 [benchmarks/results/arena-retain.json](results/arena-retain.json)。

## 字符串搜索

使用字节搜索及已有字符串比较。

状态：舍弃，收益不稳定或退化。

整体配对中位变化-0.30%，胜出2/4对。

| 轮次 | 基线 | 候选 | 变化 |
|---|---:|---:|---:|
| 1 | 3.28 | 3.29 | +0.30% |
| 2 | 3.25 | 3.27 | +0.62% |
| 3 | 3.30 | 3.27 | -0.91% |
| 4 | 3.33 | 3.17 | -4.80% |

| 子项 | 配对中位变化 |
|---|---:|
| acorn | -0.34% |
| babel | +0.78% |
| babel-minify | +1.52% |
| babylon | +0.61% |
| buble | -1.21% |
| chai | +1.37% |
| espree | -0.92% |
| esprima | -0.54% |
| jshint | -1.32% |
| lebab | +0.58% |
| postcss | -1.46% |
| prepack | -6.09% |
| prettier | -0.99% |
| source-map | +2.01% |
| terser | +1.06% |
| typescript | +1.52% |
| uglify-js | +0.21% |

完整原始数据见 [benchmarks/results/string-search.json](results/string-search.json)。

## 字符串长度

直接读取字符串长度。

状态：舍弃，收益不稳定或退化。

整体配对中位变化-4.74%，胜出1/4对。

| 轮次 | 基线 | 候选 | 变化 |
|---|---:|---:|---:|
| 1 | 3.27 | 2.98 | -8.87% |
| 2 | 3.22 | 3.20 | -0.62% |
| 3 | 3.27 | 3.28 | +0.31% |
| 4 | 3.18 | 2.52 | -20.75% |

| 子项 | 配对中位变化 |
|---|---:|
| acorn | +0.00% |
| babel | -26.78% |
| babel-minify | -4.59% |
| babylon | -1.63% |
| buble | -0.71% |
| chai | +5.55% |
| espree | -0.44% |
| esprima | -2.96% |
| jshint | -7.13% |
| lebab | -2.17% |
| postcss | -4.78% |
| prepack | -3.41% |
| prettier | -10.85% |
| source-map | -2.16% |
| terser | -1.06% |
| typescript | -0.50% |
| uglify-js | -1.33% |

完整原始数据见 [benchmarks/results/length-fast.json](results/length-fast.json)。

## 数组内嵌属性

数组对象与初始属性合并分配，三个realloc统一helper复制存活槽位，free跳过内嵌指针；第二对候选异常慢，存在并发波动；受限内存测试中基线和候选均可能出现未捕获OOM，结果不稳定，不能归因于候选。

状态：舍弃，收益不稳定或退化。

整体配对中位变化-0.76%，胜出2/6对。

| 轮次 | 基线 | 候选 | 变化 |
|---|---:|---:|---:|
| 1 | 3.21 | 3.26 | +1.56% |
| 2 | 3.15 | 2.73 | -13.33% |
| 3 | 3.34 | 3.22 | -3.59% |
| 4 | 3.29 | 3.25 | -1.22% |
| 5 | 3.32 | 3.31 | -0.30% |
| 6 | 3.28 | 3.31 | +0.91% |

| 子项 | 配对中位变化 |
|---|---:|
| acorn | +2.12% |
| babel | -1.70% |
| babel-minify | -1.07% |
| babylon | -1.20% |
| buble | -1.65% |
| chai | -1.94% |
| espree | -1.41% |
| esprima | -1.88% |
| jshint | -2.91% |
| lebab | -1.09% |
| postcss | +1.34% |
| prepack | +2.28% |
| prettier | -0.99% |
| source-map | -1.32% |
| terser | -2.05% |
| typescript | -2.21% |
| uglify-js | -0.50% |

完整原始数据见 [benchmarks/results/array-inline.json](results/array-inline.json)。

## Map数值哈希

仅混合数值及指针路径，避免字符串key额外指令开销；第四对基线异常慢，存在并发波动。

状态：实验完成；当前未纳入提交。

整体配对中位变化+0.46%，胜出4/6对。

| 轮次 | 基线 | 候选 | 变化 |
|---|---:|---:|---:|
| 1 | 3.27 | 3.25 | -0.61% |
| 2 | 3.29 | 3.30 | +0.30% |
| 3 | 3.29 | 3.32 | +0.91% |
| 4 | 2.94 | 3.18 | +8.16% |
| 5 | 3.29 | 3.22 | -2.13% |
| 6 | 3.28 | 3.30 | +0.61% |

| 子项 | 配对中位变化 |
|---|---:|
| acorn | -0.36% |
| babel | +3.33% |
| babel-minify | +4.02% |
| babylon | +0.41% |
| buble | +1.68% |
| chai | -0.97% |
| espree | -0.91% |
| esprima | +0.54% |
| jshint | -4.52% |
| lebab | -0.28% |
| postcss | -2.31% |
| prepack | +3.20% |
| prettier | -0.40% |
| source-map | -0.33% |
| terser | -0.53% |
| typescript | +0.23% |
| uglify-js | -2.22% |

完整原始数据见 [benchmarks/results/map-numeric-hash.json](results/map-numeric-hash.json)。

## 局部变量字段融合

解释器将相邻get_loc/get_arg与plain own get_field融合，失败回原语义。

状态：已集成至提交c4cc952f。

整体配对中位变化+2.38%，胜出6/6对。

| 轮次 | 基线 | 候选 | 变化 |
|---|---:|---:|---:|
| 1 | 3.35 | 3.45 | +2.99% |
| 2 | 3.36 | 3.48 | +3.57% |
| 3 | 3.36 | 3.44 | +2.38% |
| 4 | 3.36 | 3.42 | +1.79% |
| 5 | 3.35 | 3.43 | +2.39% |
| 6 | 3.35 | 3.41 | +1.79% |

| 子项 | 配对中位变化 |
|---|---:|
| acorn | +4.81% |
| babel | +2.45% |
| babel-minify | +1.90% |
| babylon | +6.64% |
| buble | +4.87% |
| chai | -0.37% |
| espree | +3.20% |
| esprima | +6.74% |
| jshint | +1.03% |
| lebab | +0.67% |
| postcss | +1.57% |
| prepack | +2.41% |
| prettier | +2.16% |
| source-map | +1.30% |
| terser | +1.03% |
| typescript | +1.98% |
| uglify-js | +1.15% |

完整原始数据见 [benchmarks/results/load-field-fusion.json](results/load-field-fusion.json)。

## 集成属性缓存

正式补丁经完整Release ThinLTO构建，与基线配对；并发阶段测量。

状态：实验完成；当前未纳入提交。

整体配对中位变化+3.49%，胜出4/4对。

| 轮次 | 基线 | 候选 | 变化 |
|---|---:|---:|---:|
| 1 | 3.36 | 3.42 | +1.79% |
| 2 | 3.27 | 3.41 | +4.28% |
| 3 | 3.33 | 3.42 | +2.70% |
| 4 | 2.98 | 3.46 | +16.11% |

| 子项 | 配对中位变化 |
|---|---:|
| acorn | +2.05% |
| babel | +1.29% |
| babel-minify | +0.42% |
| babylon | +2.21% |
| buble | +1.99% |
| chai | +3.46% |
| espree | +0.45% |
| esprima | +4.26% |
| jshint | +0.80% |
| lebab | +2.41% |
| postcss | +5.44% |
| prepack | +2.81% |
| prettier | +1.76% |
| source-map | +5.83% |
| terser | +1.04% |
| typescript | +4.30% |
| uglify-js | +1.49% |

完整原始数据见 [benchmarks/results/integrated-hint.json](results/integrated-hint.json)。

## 属性缓存加atom索引

对照是property-hint而非原始基线；六对中基线数轮明显受并发干扰。

状态：实验完成；当前未纳入提交。

整体配对中位变化+4.16%，胜出5/6对。

| 轮次 | 基线 | 候选 | 变化 |
|---|---:|---:|---:|
| 1 | 3.47 | 3.45 | -0.58% |
| 2 | 3.07 | 3.37 | +9.77% |
| 3 | 3.47 | 3.49 | +0.58% |
| 4 | 3.47 | 3.48 | +0.29% |
| 5 | 3.23 | 3.48 | +7.74% |
| 6 | 3.11 | 3.46 | +11.25% |

| 子项 | 配对中位变化 |
|---|---:|
| acorn | +0.66% |
| babel | +1.38% |
| babel-minify | +5.33% |
| babylon | +0.39% |
| buble | +0.19% |
| chai | -2.00% |
| espree | +0.00% |
| esprima | +1.28% |
| jshint | +0.46% |
| lebab | +2.11% |
| postcss | +0.52% |
| prepack | +1.95% |
| prettier | +0.20% |
| source-map | +0.16% |
| terser | +1.62% |
| typescript | -0.29% |
| uglify-js | +0.64% |

完整原始数据见 [benchmarks/results/hint-atom-combined.json](results/hint-atom-combined.json)。

## 属性缓存加局部字段融合

对照是property-hint而非原始基线；首对候选明显受并发干扰。

状态：已集成至提交c4cc952f。

整体配对中位变化+0.72%，胜出4/6对。

| 轮次 | 基线 | 候选 | 变化 |
|---|---:|---:|---:|
| 1 | 3.48 | 3.22 | -7.47% |
| 2 | 3.46 | 3.45 | -0.29% |
| 3 | 3.47 | 3.50 | +0.86% |
| 4 | 3.45 | 3.48 | +0.87% |
| 5 | 3.47 | 3.49 | +0.58% |
| 6 | 3.47 | 3.51 | +1.15% |

| 子项 | 配对中位变化 |
|---|---:|
| acorn | +2.66% |
| babel | +1.96% |
| babel-minify | +3.18% |
| babylon | +3.90% |
| buble | +1.37% |
| chai | -3.29% |
| espree | +0.44% |
| esprima | +2.02% |
| jshint | -0.22% |
| lebab | -0.90% |
| postcss | -1.36% |
| prepack | +1.79% |
| prettier | -0.76% |
| source-map | -0.16% |
| terser | -0.55% |
| typescript | -0.29% |
| uglify-js | +0.16% |

完整原始数据见 [benchmarks/results/hint-fusion-combined.json](results/hint-fusion-combined.json)。

## 借用谓词原型

解释器借用谓词值减少引用计数操作，并发阶段六对。

状态：舍弃，收益不稳定或退化。

整体配对中位变化-7.13%，胜出1/6对。

| 轮次 | 基线 | 候选 | 变化 |
|---|---:|---:|---:|
| 1 | 3.06 | 3.12 | +1.96% |
| 2 | 3.18 | 2.99 | -5.97% |
| 3 | 3.18 | 2.98 | -6.29% |
| 4 | 3.41 | 3.12 | -8.50% |
| 5 | 3.41 | 3.12 | -8.50% |
| 6 | 3.39 | 3.12 | -7.96% |

| 子项 | 配对中位变化 |
|---|---:|
| acorn | -10.10% |
| babel | -10.84% |
| babel-minify | -6.48% |
| babylon | -8.12% |
| buble | -8.73% |
| chai | -7.75% |
| espree | -5.02% |
| esprima | -12.17% |
| jshint | -4.86% |
| lebab | -7.55% |
| postcss | -6.52% |
| prepack | -7.36% |
| prettier | -4.05% |
| source-map | -12.50% |
| terser | -6.00% |
| typescript | -8.70% |
| uglify-js | -6.54% |

完整原始数据见 [benchmarks/results/borrow-predicate.json](results/borrow-predicate.json)。

## 私有字节码借用谓词

独立候选对原始基线，预热后独立六对；测试通过私有字节码与可选分配失败回退。两对RSS峰值基线731529216/731398144字节，候选729333760/729350144字节，不足以证明内存降低。1000函数合成测试中JS_ComputeMemoryUsage较基线增加：无匹配比较32032字节，每函数一个匹配比较110032字节；两者差78000字节约78字节/函数。详见 [RSS原始数据](results/borrow-quickening-rss.json) 和 [native内存数据](results/borrow-quickening-native-memory.json)。

状态：舍弃，收益不稳定或退化。

整体配对中位变化+0.59%，胜出5/6对。

| 轮次 | 基线 | 候选 | 变化 |
|---|---:|---:|---:|
| 1 | 3.42 | 3.43 | +0.29% |
| 2 | 3.43 | 3.42 | -0.29% |
| 3 | 3.41 | 3.43 | +0.59% |
| 4 | 3.42 | 3.44 | +0.58% |
| 5 | 3.41 | 3.44 | +0.88% |
| 6 | 3.39 | 3.43 | +1.18% |

| 子项 | 配对中位变化 |
|---|---:|
| acorn | +0.68% |
| babel | +0.14% |
| babel-minify | -1.43% |
| babylon | +2.57% |
| buble | +0.79% |
| chai | +1.47% |
| espree | -0.45% |
| esprima | +0.00% |
| jshint | +0.23% |
| lebab | +0.66% |
| postcss | +1.29% |
| prepack | +0.35% |
| prettier | +0.39% |
| source-map | +0.49% |
| terser | +1.12% |
| typescript | -1.66% |
| uglify-js | +0.64% |

完整原始数据见 [benchmarks/results/borrow-quickening.json](results/borrow-quickening.json)。

## 正式组合加私有字节码借用谓词

对照为已提交的属性缓存加融合组合，而非原始基线；预热后六对。

状态：舍弃，收益不稳定或退化。

整体配对中位变化-1.00%，胜出0/6对。

| 轮次 | 基线 | 候选 | 变化 |
|---|---:|---:|---:|
| 1 | 3.49 | 3.46 | -0.86% |
| 2 | 3.49 | 3.46 | -0.86% |
| 3 | 3.50 | 3.46 | -1.14% |
| 4 | 3.51 | 3.46 | -1.42% |
| 5 | 3.49 | 3.45 | -1.15% |
| 6 | 3.48 | 3.45 | -0.86% |

| 子项 | 配对中位变化 |
|---|---:|
| acorn | -0.65% |
| babel | -0.68% |
| babel-minify | -1.64% |
| babylon | +0.94% |
| buble | +0.19% |
| chai | -0.59% |
| espree | -1.31% |
| esprima | -2.72% |
| jshint | -1.21% |
| lebab | -0.26% |
| postcss | -0.50% |
| prepack | +0.52% |
| prettier | -0.76% |
| source-map | -0.32% |
| terser | -1.58% |
| typescript | -3.55% |
| uglify-js | -1.92% |

完整原始数据见 [benchmarks/results/final-quickening-combined.json](results/final-quickening-combined.json)。

## 按位点属性内联缓存

对照为c4cc952f属性缓存加字段融合；六对，第六对基线受干扰；定向验证plans=9、hits=674、misses=364、unhashed=5、retired=11。

状态：实验完成；当前未纳入提交。

整体配对中位变化-1.28%，胜出1/6对。

| 轮次 | 基线 | 候选 | 变化 |
|---|---:|---:|---:|
| 1 | 3.51 | 3.47 | -1.14% |
| 2 | 3.50 | 3.44 | -1.71% |
| 3 | 3.51 | 3.46 | -1.42% |
| 4 | 3.33 | 3.27 | -1.80% |
| 5 | 3.46 | 3.46 | +0.00% |
| 6 | 3.11 | 3.36 | +8.04% |

| 子项 | 配对中位变化 |
|---|---:|
| acorn | -0.32% |
| babel | -1.52% |
| babel-minify | +1.27% |
| babylon | -0.94% |
| buble | +0.59% |
| chai | +0.30% |
| espree | -0.89% |
| esprima | -2.26% |
| jshint | -1.11% |
| lebab | -0.52% |
| postcss | +0.25% |
| prepack | +1.65% |
| prettier | -0.76% |
| source-map | -1.61% |
| terser | -3.63% |
| typescript | -0.79% |
| uglify-js | -1.78% |

完整原始数据见 [benchmarks/results/final-site-ic.json](results/final-site-ic.json)。

## 比较与条件分支融合

对照为已提交属性缓存加融合组合；八种比较直接消费紧邻短或长条件分支，保留异常路径及interrupt poll。双方预热后六对；可能与其他候选构建或测试并行。正式test-comparison-branch.js在对照及候选均通过且强制ESLint通过（独立运行，不计入下述套件数）；原生interrupt回调次数及异常堆栈与对照一致。完整套件503通过、1跳过、1失败：TCP中止测试在原始基线亦失败。独立完整CMake构建OFF源码与候选逐字一致，3项回归通过；ON构建及inspector/branch回归通过，重设OFF后重建通过；编译含ThinLTO且无PGO。

状态：实验完成；当前未纳入提交。

整体配对中位变化+1.71%，胜出6/6对。

| 轮次 | 基线 | 候选 | 变化 |
|---|---:|---:|---:|
| 1 | 3.49 | 3.52 | +0.86% |
| 2 | 3.51 | 3.55 | +1.14% |
| 3 | 3.50 | 3.57 | +2.00% |
| 4 | 3.50 | 3.56 | +1.71% |
| 5 | 3.50 | 3.56 | +1.71% |
| 6 | 3.49 | 3.56 | +2.01% |

| 子项 | 配对中位变化 |
|---|---:|
| acorn | +1.96% |
| babel | +0.81% |
| babel-minify | +0.81% |
| babylon | +1.88% |
| buble | +2.67% |
| chai | -0.73% |
| espree | +2.63% |
| esprima | +0.49% |
| jshint | +1.68% |
| lebab | +1.03% |
| postcss | +3.01% |
| prepack | +0.17% |
| prettier | +1.70% |
| source-map | +2.56% |
| terser | +1.12% |
| typescript | +7.58% |
| uglify-js | +1.12% |

完整原始数据见 [benchmarks/results/comparison-branch.json](results/comparison-branch.json)。

## hint+fusion 阶段的三方确认（c4cc952f）

属性缓存加局部字段融合已提交为 c4cc952f。原始基线、仅属性缓存和属性缓存加融合，各预热一套；6轮交错顺序运行，独立测量。每项为同轮吞吐比的中位数，正值代表该阶段hint+fusion版本提升。

| 指标 | hint+fusion / 原始基线 | hint+fusion / 仅属性缓存 |
|---|---:|---:|
| acorn | +4.73% | +1.32% |
| babel | +3.11% | +0.27% |
| babel-minify | +2.28% | +0.52% |
| babylon | +5.56% | +2.70% |
| buble | +4.37% | +3.13% |
| chai | +0.00% | -3.29% |
| espree | +2.70% | +1.33% |
| esprima | +7.71% | +2.78% |
| jshint | +0.45% | +0.23% |
| lebab | +1.98% | -0.26% |
| postcss | +2.30% | -0.50% |
| prepack | +0.53% | +0.36% |
| prettier | +0.77% | -0.57% |
| source-map | +1.13% | -0.47% |
| terser | +1.12% | +0.00% |
| typescript | +2.26% | +0.29% |
| uglify-js | +1.79% | +0.00% |
| Geometric mean | +2.35% | +0.58% |

hint+fusion版本相对baseline总体胜出6/6轮。

hint+fusion版本相对hint总体胜出5/6轮。

完整原始数据见 [final-triad.json](results/final-triad.json)。
