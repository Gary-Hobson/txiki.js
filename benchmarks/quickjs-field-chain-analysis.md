# 连续自有字段访问的借用区间

2026-11-10。对照是fa509604正则裁剪后的正式版本，Release + ThinLTO，关闭PGO和inspector。本原型只扩展既有`js_load_local_or_field`，不添加访问点表、shape身份、私有opcode或函数元数据。

原实现融合`get_loc/get_arg → get_field`一次，返回一个拥有引用的字段值。后续紧邻`get_field`会再复制下一值、释放中间值并执行一次主VM分派。新实现沿连续的普通自有数据属性向下读取，直到遇到不同指令、非对象、缺失属性或非普通数据属性，再仅为最终值增加引用。这扩展无副作用的借用区间，避免为每一个中间对象产生临时所有权。

## 生命周期与回退边界

local或argument持有根对象，每个已通过检查的普通属性又持有下一值的强引用。整个循环不调用JavaScript、不分配、不释放值、不中断，因此根到中间对象的强路径在最后一次dup之前不会消失。循环和自引用不改变此证明。子代理对这个不变量进行了独立源码审查。

每一步使用原融合已有的`find_own_property`与`JS_PROP_TMASK`检查，没有扩大单步允许的属性语义。遇getter、原型查询、Proxy路径或primitive时停止，先dup当前前缀的结果，再把原PC交回后续`get_field`。因此getter即使改写根或前一属性，当前receiver仍已取得独立所有权；已完成的普通读取不重复，异常PC仍指向原指令。

所有canonical字节码和分支目标保持不变。ENABLE_DUMPS仍使用既有早退；inspector插入的指令自然中断连续字段序列，但是否正式保留后仍需实际构建验证。此改动不额外跨越poll；它覆盖的原plain-own访问本身无poll。

## 同次运行的覆盖诊断

独立诊断版本在原型helper中记录实际完成的字段深度，并计数主VM SWITCH。深度统计0表示未融合字段，1表示只有原本已支持的一次融合；8桶包含所有大于等于8的深度。本次全WTB运行结果：

| 字段深度 | helper调用次数 |
|---|---:|
| 0 | 977,768,150 |
| 1 | 184,261,472 |
| 2 | 37,002,158 |
| 3 | 464,167 |
| 4 | 142 |
| 5及以上 | 0 |

相对原单字段融合，实际额外消除37,930,918次字段dispatch。同run实际主VM分派4,118,873,785，重建原方案参照4,156,804,703，额外减少约0.9125%。这不代表相同比例的CPU时间或缓存命中改善；循环检查、寄存器使用和编译布局仍可能抵销被省下的操作。统计使用诊断版，不将其吞吐计分，也不与历史运行的分派总数相除。

## 当前验证

原型、诊断版与对照分别通过独立field-chain专项、已有load-field-fusion与property-cache回归。专项覆盖普通链、根/中间getter改变上层引用、Proxy观察、原型与删除、自循环、冻结对象、各深度异常、最终对象身份、generator恢复以及1024层连续字段。

双方各完整预热、六对AB/BA独立计分后，总分配对中位 **−3.23%**，0/6胜，全部17项中位均下降。此循环内联版本拒绝接入正式补丁。

| 项目 | 配对中位变化 |
|---|---:|
| acorn | -4.63% |
| babel | -3.47% |
| babel-minify | -2.46% |
| babylon | -1.30% |
| buble | -3.96% |
| chai | -3.27% |
| espree | -3.82% |
| esprima | -4.50% |
| jshint | -1.96% |
| lebab | -3.83% |
| postcss | -2.88% |
| prepack | -1.90% |
| prettier | -2.24% |
| source-map | -6.46% |
| terser | -2.16% |
| typescript | -5.32% |
| uglify-js | -2.66% |
| Geometric mean | -3.23% |

原分及指纹：[field-chain.json](results/field-chain.json)、[metadata](results/field-chain-metadata.json)、[诊断](results/field-chain-diagnostics.json)。[归档变换](experiments/field-chain-transform.py)由replay工具重建，输出与测试源码一致；[专项](experiments/field-chain-correctness.js)保持实际执行内容。

## 代码布局证据与一次针对性后续

对两个实际计分二进制执行`xcrun nm -an`，`JS_CallInternal`至下一text函数符号的跨度从46,248增至53,644字节（含潜在对齐）；两者均无独立`js_load_local_or_field`符号。它表明循环实现改变了主解释器的编译布局，但不是指令缓存miss或寄存器spill的实测，也不能独自确定−3.23%的因果。

第二个候选只把多字段续读循环移至显式no_inline helper，保留原单字段快路，仅在第二个字段访问时调用尾部helper。该设计针对主解释器循环复制的可疑成本，不改变语义域、字段查找方法或生命周期不变量；它自身仍可能因额外判断和调用而变慢。已完成下面的独立验证与计分。

## 第二版：独立helper仍失败

[outlined变换](experiments/field-chain-outline-transform.py)输出与测试源码逐字一致；同一组三项专项通过。实际二进制有独立`js_load_field_tail`符号，主解释器到下一text函数的符号跨度为46,488字节，仅比对照增加240字节。它恢复了接近原有的主解释器体积，却增加了字段链尾部的C调用。

同fa509604与body-only字符候选一起各预热后，六轮覆盖全部排列。outlined版相对fa509604总分配对中位 **−9.79%**，0/6胜；17项在所有轮次均落后。这否定了“把循环移出去就能恢复性能”的具体修正，并不证明所有字段融合方案无效。不能把首版回退完全归因于代码体积；没有硬件计数或逐条动态成本分解时，不给cache miss、spill或新增C调用分配虚构比例。两版均舍弃，停止本轮连续字段候选。

| 项目 | outlined配对中位变化 |
|---|---:|
| acorn | -17.98% |
| babel | -13.21% |
| babel-minify | -13.23% |
| babylon | -15.47% |
| buble | -11.48% |
| chai | -1.99% |
| espree | -16.71% |
| esprima | -19.55% |
| jshint | -4.02% |
| lebab | -4.45% |
| postcss | -6.22% |
| prepack | -9.22% |
| prettier | -5.06% |
| source-map | -5.40% |
| terser | -6.75% |
| typescript | -8.30% |
| uglify-js | -4.43% |
| Geometric mean | -9.79% |

完整三方[原分](results/body-outline-triad.json)、[元数据](results/body-outline-triad-metadata.json)、[配对统计](results/body-outline-triad-summary.json)。本报告只讨论outlined/anchor比较，body字符快路的决定单独记录。
