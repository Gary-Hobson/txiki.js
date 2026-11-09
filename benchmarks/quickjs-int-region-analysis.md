# 整数值流区域解释器实验

2026-11-10。**当前原型按覆盖门槛拒绝，不运行六对，也不宣称实测吞吐负收益。** 正确性和完整WTB诊断已完成；净减少主VM分派约0.079%，覆盖不足以支持本轮架构目标。输入基线是已保留的 `integrated-object-layout`（hint、load-field fusion、comparison-branch fusion、shared object layout），正式提交52becd9e。本实验不改正式 patch 或 submodule。

源码：[int-region-transform.py](experiments/int-region-transform.py)。变换产生独立 `stage-region/quickjs.c`。归档replay的 `object-int-region` 输出与实际验证候选逐字相同，SHA-256为 `233e7987e9001d28d6adaa713901031848e23569556b5d90766516dbcba56160`；见 [重建证据](experiments/int-region-replay-evidence.json)。

## 1. 可检验的架构假设

把无回调的整数基本块片段编译成一个小型 DAG，以输入寄存器和虚拟写集执行。它要消除多个 `get_loc/get_arg`、常量push、dup、store、JSValue16字节中间结果及相邻运算的重复类型判断，不再为每一步操作通过完整 VM dispatch。计算节点仍由可移植 C switch 执行，不声称彻底消灭派发或构造了 JIT。

例如 `var a=x+y; var b=a*3; a=b-x; b=a^y;` 中，每个参数只检查/提取一次，后续运算处理 int32；`a` 和 `b` 的后续读使用虚拟写集最新版本。至少6条原指令、至少2次计算、至少1次数值计算才形成区域。单个 load-field predicate 不会入选，本原型也完全不包含属性查询。

原基线对应源码（未打patch的 `deps/quickjs/quickjs.c`）中，`get_loc/get_arg` handler 为每次读取产生一个 JSValue，`set_value` 执行替换值的所有权操作；整数算术 handler 已有快速类型判断，但各个运算独立读取/写回VM栈。新方案省的是一段内的这些重复工作，不能把 `JS_CallInternal` 的采样占比全归给它。

[原完整opcode诊断](results/opcode-profile.json) 的4.706B dispatch中，add/sub/mul/shl/sar/shr/and/or/xor/neg/not 合计49.61M，inc_loc/dec_loc/add_loc 合计54.63M，比较合计396.52M。数值部分明显小于属性访问和所有比较的总规模。`get_length→lt` 47.98M、`push_i8→strict_eq` 39.30M、`inc_loc→goto8` 40.90M等常见形态，并不因为热门就满足当前区域边界：前者包含不支持的属性/长度producer，中者通常只有一项计算，后者可能只是独立循环更新。不能把这些动态相邻对当成有足够长度的可融合基本块；它们还可能跨调用。

每个命中区域最多32条原指令，理论最多省31次VM分派；实际区域长度、整数成功率、捕获变量排除率和块内副作用屏障决定覆盖。仅依据旧opcode边际分布无法给出有用的性能上限，尤其不能把396.52M比较都归入收益。此次诊断会在**同一次运行**计数原工作量，而不以新版本的计数除旧版本4.706B来宣称覆盖率。

## 2. 数据结构与执行流程

每个 JSFunctionBytecode 增加一个计划指针，冷函数也承担其sizeof成本，需测量。没有增加所有函数调用或所有get_loc的执行检查，没有长度为bytecode_len的常驻map，也不保存整段字节码副本。

计划只在有合格区域的函数分配，包含排序的site数组和紧凑payload。每个site保存起止PC、payload偏移、输入/计算/写集计数、结果类别、原入口5字节；payload包含8字节输入、4字节计算节点、4字节写集项。输入最多16、计算节点最多16、写入局部最多8，虚拟栈最多8。相同原始local/arg/常量输入去重，相同运算及输入的计算节点可复用。局部写建立新值版本，之后不再错误复用初始值输入。

只在非inspector构建使用私有opcode253，入口后4字节保存site编号，做到直接定位。正常OP_COUNT=252，正式对象布局占254/255；inspector额外的debug/debugger_stmt占252/253，因此opcode定义、dispatch表、handler和prepare均由quickjs.h的debugger宏隔离，不能只在运行时不启用。保留静态断言检查正常opcode余量，ENABLE_DUMPS也禁用prepare。

构建/反序列化完成后，先执行已有object-layout prepare，再扫描整数区域。扫描器将254/255只视作长度1/5的object/define_field屏障，不解读其私有operand，不以私有编号直接索引OP_COUNT数组。区域与对象计划的改写位置互不重叠。

入口保持byte_code_buf原地址，保存被覆盖的5字节后写253和site编号。没有入口热计数，也没有切换运行中frame的PC基址。整段不包含call、yield、await、branch或其他可重入点，既有调用者、异常定位和PC偏移继续使用原字节地址。

入口执行分为三阶段：

1. 检查全部写入目标原值为INT或UNDEFINED；读取去重后的输入并检查非constant输入均为INT。
2. 用局部int32寄存器数组计算DAG，不写VM栈或局部值；每个溢出/负零/超域结果都在这一阶段失败。
3. 所有检查成功才提交虚拟写集，并按结果类别push一个int/boolean或不push。之后回到原字节码后继。末端比较可复用已有comparison-branch融合，并在原位置执行一次poll。

任一失败都会先恢复此site的5字节入口，永久退役该site，随后以原pc/sp/locals执行整个原段。失败不重跑已经发生的JS副作用，因为guard/计算阶段没有这些副作用，也尚未提交写入。退役避免不稳定类型在每次执行时反复支付guard；代价是第一次失败后不再尝试重特化，本轮不增加自适应策略。

## 3. 语义和生命周期不变量

**局部旧值与释放。** 即使local只写不读，其旧值仍需通过INT/UNDEFINED检查。不能把原put_loc释放对象/符号等的行为统一延后；一旦旧值有这种可能，整段回退。成功路径的所有输入、结果和被替换旧值均无refcount操作，不能调用finalizer、分配或触发GC。

**捕获变量。** 编译器 `capture_var:33923` 首次捕获时设置is_captured并增加函数的var_ref_count；编译在37150保存该计数，序列化38874和反序列化39811保留它。运行时 `get_var_ref:17531` 使用预计算var_ref_idx。原型拒绝var_ref_count非零的整个函数，另拒绝eval/apply_eval、make_loc_ref/make_arg_ref、with操作和mapped arguments；get_loc_check等TDZ操作不是支持指令。外部closure_var不被本IR读取或写入；其存在本身不要求拒绝，但函数自身被捕获的local/arg已按上述计数排除。generator/async整函数拒绝，避免恢复帧和额外所有权证明。

**控制流和poll。** 扫描所有label格式建立临时目标表，区域内部不能有任何跳入目标。所有支持的load/constant/dup/drop/swap、局部store和成功整数运算原路径没有interrupt poll；goto、if和call是边界。原goto/if在19171–19263的poll保留；末端比较若消耗已有融合的branch，先提交与原路径相同的结果/locals，再更新pc、弹出boolean并调用原poll。比较的slow path可能回调或分配，但只有INT输入可进入本计算阶段，其他类型整段回退。

**数值边界。** add/sub/mul使用int64计算并检查int32范围；mul的零且负符号以及neg(0)拒绝，保留负零；neg(INT_MIN)、inc/dec溢出拒绝。shl先转uint32，移位量掩码31；sar沿用原实现的有符号右移语义；shr结果大于INT_MAX回退。div/rem/pow不支持，避免除零、INT_MIN/-1、负零及其他数值域扩大。比较必须是区域最后一项计算，输出JS_TAG_BOOL，绝不让后续算术或strict_eq把boolean当作INT。

**canonical与GC。** 序列化先复制执行字节，再恢复整数入口和object-layout字节；free在atom扫描前恢复两套改写。两种计划各自只恢复自己的位置。输入是无引用的kind/index/constant，计划不持有JSValue/atom/shape，因此无额外mark边。计划在创建/读取时零初始化，分配失败使用rt级分配接口直接放弃优化，不设置JS异常；free和JS_ComputeMemoryUsage纳入常驻计划。

## 4. 诊断口径及待验证门槛

计分版本不启用 `QJS_INT_REGION_DIAGNOSTICS`。诊断版记录scanned_functions、plans、sites、累计bytes、live/peak bytes、calls、hits、退役及type/write/arithmetic失败，并在GC时打印。bytes是累计计划分配，live/peak只统计计划payload，不包含JSFunctionBytecode新增指针或扫描临时target数组；后两者须以sizeof/实际内存另看。

`covered_insns`为成功区域覆盖的原dispatch数，`saved_dispatches`累加每次命中的insns-1。get_loc0_loc1本来只有一次dispatch，因此记1；末端if已由已保留comparison-branch融合处理，不额外计入。诊断同时计数实际VM dispatch次数。每个失败入口多执行一次253，然后退役，所以同run参照数为：

```
reference_dispatches = actual_dispatches + saved_dispatches - retired
net_dispatches_removed = saved_dispatches - retired
```

这只重建对应执行中的主VM SWITCH次数，不是减少同样数量的CPU间接分支、全部dispatch开销或时间份额。IR仍有inputs/nodes/writes循环和node-op switch，`values[48]`需要192字节C栈临时，输入guard、metadata读取、提交和每次helper调用也仍有成本；这些成本完全可能抵销减少的VM栈读写和SWITCH。

专项：[int-region-correctness.js](experiments/int-region-correctness.js) 覆盖SSA多次读写、重复表达式、类型/负零/溢出回退、晚coercion异常和赋值次序、旧对象write guard、分支目标、captured/mapped/eval/TDZ/generator/async排除及与对象计划共存。Native [int-region-native.c](experiments/int-region-native.c) 覆盖递归活跃帧canonical序列化、读取执行以及循环正常/中止poll节奏。baseline、候选、诊断版均通过这些测试和既有object-layout/private-bytecode专项；native正常poll均为4、主动中止均为2。临时target数组分配和计划分配分别强制失败，也均无JS异常回退通过。另由主代理独立编写的 [差分fixture](experiments/int-region-differential-parent.js) 在baseline/候选产生4320个返回值/异常结果，stdout逐字相同。

纯源码replay已通过 region→debugger 和 debugger→region 两种patch顺序；这不等于实际inspector ON编译/执行通过。首个native fixture编译问题是测试回调用了JSRuntime*，已按当前JSInterruptHandler签名修为JSContext*，候选核心未改。

## 5. 完整WTB诊断与裁决

计数版只用于覆盖分析，原始方法和计数保存在 [int-region-diagnostics.json](results/int-region-diagnostics.json)，不使用其中插桩运行的吞吐作为性能分数：

| 指标 | 数值 |
|---|---:|
| 已通过函数排除且含数值op的扫描函数 | 10,671 |
| 有计划的函数 / sites | 383 / 630 |
| 计划累计 / 当前 / 峰值字节 | 53,396 / 53,396 / 53,396 |
| 私有入口调用 / 命中 / 退役 | 591,057 / 591,053 / 4 |
| type / write / arithmetic guard失败 | 1 / 0 / 3 |
| 命中覆盖的原指令 | 3,872,129 |
| saved_dispatches | 3,281,076 |
| 实际主VM分派 | 4,155,486,294 |
| 同run重建参照分派 | 4,158,767,366 |
| 净减少主VM分派 | 3,281,072（0.07890%） |

每次命中平均覆盖6.55条原指令，接近6条最低门槛；只有扫描函数中的3.59%产生计划。命中率接近100%只能说明已经挑出的纯整数片段稳定，不能代表整个workload被覆盖。计划每site平均约84.8字节，且53,396字节不包含所有函数新增计划指针或临时扫描开销；未额外构建测量JSFunctionBytecode sizeof。

本轮证明了事务式值流执行的语义原型可以工作，**没有证明它具有可观的WTB收益**。净减少约0.079%的主VM SWITCH还需支付输入/节点/写集循环、helper栈临时和元数据访问成本，因此按预设覆盖门槛停止。该百分比不是时间占比或严格Amdahl上限；未进行配对计时，不能写成吞吐退化。后续分析应先定位实际热块的结构屏障，而非降低安全门槛或把单个谓词重新包装成大区域。
