# QuickJS 整机执行成本与架构路线

2026-11-10，针对 txiki.js 的 Web Tooling Benchmark。分析基准：Apple M4、Release + ThinLTO、无 PGO、无 inspector。本文只做源码、已有产物和已有采样分析，未编译、未运行 benchmark。源码行号基于 `deps/quickjs` 的 `7b5f588e0f9b643e2996f291bfc861abeaceddc2`；性能 patch 在构建副本应用，因此构建副本行号可能不同。

结论：最有根据的主线是 **按访问点收集反馈并建立可靠的布局身份，然后把热基本块降低为带所有权约束的值流 IR**。前者省去重复的语义判定，后者一起减少栈搬运、引用计数和 dispatch；两者也能成为以后可选 JIT 的公共基础。第三条有条件的路线是 **普通对象 literal 的布局预规划**，同时消除创建过程中逐字段的 shape 演进和属性数组扩容。当前没有证据支持先重写 cycle GC、启用 64 位 NaN boxing，或直接替换全部字节码为寄存器 VM。

这些不是现有小实验的重述：1024 项 atom 索引 slot hint、atom padding 缓存、数组对象与 props 合并分配，以及紧邻 `get_loc/get_arg → get_field` 的动态融合，均视为已经探索的基础，不计入本文新路线的成果。

## 1. 证据能证明什么

### 1.1 已有实测

实验记录见 [quickjs-optimization.md](/Volumes/zhitai/workspace/lite-engine/txiki.js/benchmarks/quickjs-optimization.md)。分析启动时，slot hint 八对七胜、配对中位数约 +2.70%，17 子项均为正；atom-index 四对四胜、约 +0.75%，仍需确认。内联、hash、尺寸查表、字符串搜索没有稳定的系统收益。这个结果说明“减少重复查找工作”值得继续，但不能推导完整 IC 会按比例增益，也不能把独立实验的百分数相加。

分析期间收到新增实测：单独 load-field-fusion 六对六胜，中位数 +2.38%，最差子项 chai -0.37%；getter 改 local/arg、Proxy、异常与自引用用例通过，和 slot hint 的组合仍在验证。这增强了“跨操作消除 ownership 往返”路线的依据，但只覆盖相邻两指令，不证明整区域设计已经实现。array-inline 六对中位数 -0.76%，按性能舍弃；低内存压力行为在基线和候选都不稳定，不能归因成候选独有缺陷。并行执行器的筛选结果仍需按实验记录的方法限制解读。

### 1.2 新的 10 秒 sample 窗口

原始文件：[baseline-sample.txt](/Volumes/zhitai/workspace/lite-engine/txiki.js/build/lto-benchmark/quickjs-hotpaths/baseline-sample.txt)。报告记录 1 ms 周期，2026-11-10 02:17:32.413 +0800 开始，进程启动于 02:17:31.348。主线程有 8522 个样本；四个 libuv worker 各 8522 个，全部等待，汇总中的 34088 个 `__psynch_cvwait` 必须排除。

| 主线程栈顶符号 | 样本数 | 占主线程样本 |
|---|---:|---:|
| JS_CallInternal | 5313 | 62.34% |
| lre_exec | 797 | 9.35% |
| JS_GetPropertyInternal | 229 | 2.69% |
| js_strict_eq2 | 208 | 2.44% |
| js_arena_malloc | 131 | 1.54% |
| js_free_rt | 124 | 1.46% |
| js_call_c_function | 109 | 1.28% |
| JS_SetPropertyInternal2 | 108 | 1.27% |
| JS_NewObjectFromShape | 90 | 1.06% |
| JS_RunGC | 14 | 0.16% |

对调用树按 `self = 节点计数 − 直接子节点计数之和` 计算，所有主线程 self 之和恰好为 8522，与报告栈顶汇总一致。去掉同名递归重复后的 inclusive 计数：`lre_exec` 834（9.79%）、`add_property` 319（3.74%）、`free_gc_object` 161（1.89%）、`JS_NewObjectFromShape` 160（1.88%）、`JS_RunGC` 20（0.235%）。这些 inclusive 集合可能重叠，不能相加。

**限制非常重要：**完整 WTB 进程被启动，不等于这 10 秒覆盖了全部 17 子项。窗口从启动约 1.065 秒后开始，没有子项 phase 标记；不能把 62.34% 称为整套 WTB 的解释器比例。`JS_CallInternal` 内还有内联的 own-property 查找、RC、类型检查、算术和分支，62.34% 更不能叫“dispatch 占比”。sample 也不是 PMU，不能从这里断言 cache miss 或 branch miss 比例。

这个窗口支持的优先级是：解释器内部值流值得拆解，cycle GC 暂无优先重写的证据；正则 VM 可能是另一个有意义的子系统，但需要按子项确认，本文不扩展其改造。

### 1.3 已有 PGO 计数仅作频率证据

`build/lto-benchmark/pgo/experiment.py:34` 显示训练是三次完整 WTB 运行。`tjs.profdata` 是 IR instrumentation、`entry_first = 0`。`llvm-profdata show --topn` 输出的是 **函数内部最大 block count**，不是函数调用次数，也不是耗时。例如：

| 函数 | 最大内部 block count（三次训练合计） |
|---|---:|
| lre_exec | 3,146,441,811 |
| JS_CallInternal | 1,113,152,295 |
| resolve_labels | 492,688,908 |
| js_strict_eq2 | 478,606,479 |
| js_arena_malloc | 443,923,216 |
| JS_GetPropertyInternal | 338,505,417 |
| JS_SetPropertyInternal2 | 230,214,471 |
| add_property | 148,669,080 |
| JS_NewObjectFromShape | 74,304,807 |
| resize_properties | 46,740,036 |

`remarks/pgo-quickjs.log` 对 `js_clone_shape` 内 memcpy 给出 hotness 21,940,692，对 `resize_properties` 的 shape/hash 搬迁 memcpy 给出 9,583,182；调用点和 inline 克隆会重复出现，不能全部累加。它们证明形状克隆与扩容不是罕见冷路径，却不能确定可节省多少时间。`resolve_labels` 的高计数还提醒我们：训练含 bundle 编译/启动，必须将计时内 JS 工具的解析工作与引擎加载编译区分开。

PGO 不是架构优化的上限。它能改变 C 级代码布局、内联和分支权重，无法自动建立 JS 访问点反馈、证明借用值生命周期、删除跨 handler 的所有权转移，或预规划 JS 对象布局。

### 1.4 完整运行的 opcode 诊断补充

后续独立诊断产物见 [opcode-profile.md](/Volumes/zhitai/workspace/lite-engine/txiki.js/benchmarks/results/opcode-profile.md) 与 [opcode-profile.json](/Volumes/zhitai/workspace/lite-engine/txiki.js/benchmarks/results/opcode-profile.json)：整个 runtime 至 WTB 完成合计 4,705,698,147 次 dispatch；`get_field` 349,237,622（7.42%）、`get_field2` 153,928,305（3.27%）、`strict_eq` 182,815,712（3.88%）、`if_false8` 381,809,496（8.11%）。相邻对 `strict_eq→if_false8` 113,408,501、`get_field→get_field` 42,664,313、`get_field→get_loc8` 42,075,580，说明多步字段/谓词序列值得具体探测。

这份计数包含 runtime 启动/加载而未按子项切片，连续 pair 也可能跨函数调用；它不能直接证明任意多个 pair 同时组成同一个可优化区域，更不是耗时。即使能消除所有 `strict_eq→if_false8` 的其中一次 dispatch，也最多消除这份运行的 2.41% dispatch，吞吐变化须另测。优先借助它筛选完整语法、类型和所有权都合格的区域，而非把 opcode 排名当最终收益排名。

### 1.5 后续区域原型的反证与边界

后续按本文 B 路线实现了受限的多级字段/严格比较区域，完整设计与官方 Java/JIT 来源见 [quickjs-jit-inspired-analysis.md](/Volumes/zhitai/workspace/lite-engine/txiki.js/benchmarks/quickjs-jit-inspired-analysis.md)。首版在全部 local/arg load 入口查询区域 map，虽通过专项正确性测试，六对 WTB 配对总分中位 **-7.13%**、1/6 胜，已淘汰。这是对“值流区域可以不计框架税就获益”的直接反证，不支持宣称任何架构倍率。

唯一针对性修正版将额外逻辑移到已识别区域的私有 opcode 入口，保持普通 load handler 原样；构建、双向字节码互读、序列化字节一致、异步/generator 自身字段谓词、异常定位及精确计划分配失败回退已通过。独立六对中位 **+0.59%**、5/6 胜，typescript 与 babel-minify 子项约 -1.66%/-1.43%；未验证相对已保留 hint/fusion 的增量。这证明了一个可执行原型的边界，没有实现完整寄存器 IR、shape IC 或逃逸分析，也未达到大幅提升。本文各路线仍是有拒绝条件的设计；若增量不成立，就停止该候选。

## 2. 当前成本模型

定义每个子项每次固定输入执行时间：

`T = T_decode/dispatch + T_value-flow + T_semantics + T_call/frame + T_alloc/layout + T_RC-destruction + T_cycleGC + T_native + T_other`。

这些是设计分类，不能直接将符号 self 时间机械填入；同一 C handler 同时承担多个类别。计数近似模型可以写成 `Σ N_k × c_k + T_memory_stall`，但 `c_k` 依赖形状、类型、缓存及路径，内存停顿又与前面的操作重叠，因此最终必须由受控变体实测验证。

| 成本源 | 实际源码依据 | 为什么局部内联不足 |
|---|---|---|
| 栈值复制与 RC | [js_dup:1662](/Volumes/zhitai/workspace/lite-engine/txiki.js/deps/quickjs/quickjs.c:1662)、[get_loc/get_arg:18897](/Volumes/zhitai/workspace/lite-engine/txiki.js/deps/quickjs/quickjs.c:18897)、[JS_FreeValueRT:7197](/Volumes/zhitai/workspace/lite-engine/txiki.js/deps/quickjs/quickjs.c:7197) | 引用值 load 要写对象/字符串前面的 ref_count，再写 16 字节栈槽；消费后又读改写 ref_count。内联不消除这次所有权往返。 |
| dispatch | [JS_CallInternal:18054](/Volumes/zhitai/workspace/lite-engine/txiki.js/deps/quickjs/quickjs.c:18054)、[DIRECT_DISPATCH:18077](/Volumes/zhitai/workspace/lite-engine/txiki.js/deps/quickjs/quickjs.c:18077) | 已支持 computed goto，不能把“改成 threaded dispatch”作为新架构建议。每个 load/shuffle/operation 仍独立执行。 |
| 属性读写 | [get_field:19562](/Volumes/zhitai/workspace/lite-engine/txiki.js/deps/quickjs/quickjs.c:19562)、[put_field:19657](/Volumes/zhitai/workspace/lite-engine/txiki.js/deps/quickjs/quickjs.c:19657)、[find_own_property:6838](/Volumes/zhitai/workspace/lite-engine/txiki.js/deps/quickjs/quickjs.c:6838) | receiver → shape → bucket → descriptor → props → value；随后读值增引用、receiver 减引用。slot hint 已缩短其中一段，却没有访问点类型和布局知识。 |
| 调用与 frame | [frame 建立:18135](/Volumes/zhitai/workspace/lite-engine/txiki.js/deps/quickjs/quickjs.c:18135)、[call_method:18584](/Volumes/zhitai/workspace/lite-engine/txiki.js/deps/quickjs/quickjs.c:18584)、[清理:20976](/Volumes/zhitai/workspace/lite-engine/txiki.js/deps/quickjs/quickjs.c:20976) | 动态 alloca、局部槽初始化、递归 C 调用、形参补齐、返回清理都存在。内部调用常已直接复用调用者 argv，不能再次声称“首次避免参数拷贝”。 |
| 对象创建与增长 | [JS_NewObjectFromShape:6138](/Volumes/zhitai/workspace/lite-engine/txiki.js/deps/quickjs/quickjs.c:6138)、[add_property:10156](/Volumes/zhitai/workspace/lite-engine/txiki.js/deps/quickjs/quickjs.c:10156)、[resize_properties:5847](/Volumes/zhitai/workspace/lite-engine/txiki.js/deps/quickjs/quickjs.c:5847) | 对象、props 分配；按 1.5 倍容量增长；shape 全局查找/克隆/重建 hash；多次复制同一对象未来的字段。 |
| 销毁与 cycle GC | [free_object:7052](/Volumes/zhitai/workspace/lite-engine/txiki.js/deps/quickjs/quickjs.c:7052)、[mark_children:7257](/Volumes/zhitai/workspace/lite-engine/txiki.js/deps/quickjs/quickjs.c:7257)、[JS_RunGC:7485](/Volumes/zhitai/workspace/lite-engine/txiki.js/deps/quickjs/quickjs.c:7485) | RC 为零时逐字段释放；cycle GC 则遍历全对象图并减/加引用恢复。两者不能混称 GC。 |

64 位 `JSValue` 是 union 8 字节 + int64 tag，共 16 字节，见 [quickjs.h:327](/Volumes/zhitai/workspace/lite-engine/txiki.js/deps/quickjs/quickjs.h:327)。普通 `JSProperty` 也是 16 字节，因为 getter/setter 和 autoinit 都要两个指针，见 [quickjs.c:1083](/Volumes/zhitai/workspace/lite-engine/txiki.js/deps/quickjs/quickjs.c:1083)。所以仅把 value 改成 8 字节，**并不自动让普通对象的 props 减半**；必须另改 descriptor payload 布局。数组 elements、栈、常量池则会直接受影响。

当前 arm64 源码布局下 `JSObject` 主体预计 72 字节，加 allocator header 8 字节；两个初始属性为 32 字节、另加 8 字节分配头。shape 主体预计 56 字节，4 个 bucket 16 字节、两个 descriptor 16 字节，再加分配头 8 字节；共享 shape 应按共享次数摊销，不能每个对象重复计算。这里是按声明和 ABI 对齐规则推导，新增结构必须用 `sizeof/offsetof` 输出核实。对象头和 RC 已合入 arena 分配头，现有 small-block arena 也是基础设施，不应重新包装为新方案。

## 3. 路线 A：按 PC 的反馈、布局版本与 quickening

这是优先可验证的中尺度改造。它是后续值流 IR 的语义基础，但不预设必须上 JIT。

### 3.1 数据结构与执行路径

在 [JSFunctionBytecode:857](/Volumes/zhitai/workspace/lite-engine/txiki.js/deps/quickjs/quickjs.c:857) 增加一个惰性 `JSExecPlan *`。计划包含 canonical-PC 到 feedback site 的映射、少量 IC entries、热度/失败计数；未来再容纳 region IR。所有 closure 共享同一 bytecode 的反馈，必须承认不同 closure/context 值可能造成多态。

第一版 entry：`{ uint64_t layout_id; uint32_t slot; uint8_t kind; }`，最多两项。状态是 EMPTY → MONO → POLY2 → MEGA；高失败率或频繁布局变化进入 MEGA，直接回现有 slot-hint/generic 路径，不能每次反复重建。准入以调用次数/回边饱和计数触发，冷函数不分配整套反馈。

首版仅支持 `class_id == JS_CLASS_OBJECT && !is_exotic` 的自有普通数据属性；读与写分别统计。读命中直接 `js_dup(p->prop[slot].u.value)`；写命中只允许原有 `(TMASK | WRITABLE | LENGTH) == WRITABLE` 语义，保持 `set_value` 的“先放新值，再释放旧值”顺序。失败执行原有 helper，成功且稳定后更新反馈。初版不得缓存 getter 返回值、函数值、JSProperty 指针、负查询或原型查询。

验证 ID 后可以省去 atom/bounds/descriptor 重查，才是相比 slot hint 的新增价值。调试构建仍重查并 assert 一致。若正式版还需要每次完整重查，先比较它与现有 hint 的净差，不把多了一层侧表当作成果。

### 3.2 为什么 shape 指针 guard 不成立

`add_property:10172` 允许唯一引用的 hashed shape 原地追加；`js_shape_prepare_update:11186` 对非 hashed shape 不克隆，对唯一 hashed shape 只是摘出 hash 表。删除、flags 转换、prototype 修改都可保持同一地址。GC/arena 还会复用地址。缓存强引用 shape 虽然减少回收问题，却会把 `ref_count==1` 变成共享，触发本来不需要的克隆，改变整个构造成本；它也不能独自解决 prototype 后续变化。

建议 runtime 持有单调 `next_layout_id`，每个 shape 记录 64 位 `layout_id`。ID 不重用，0 表示不可缓存。entry 只存 ID 和 slot，不持有 shape/object，消除 dangling pointer 和 cache 引入的额外 GC 边。计数溢出时保守永久关闭此 runtime 的 IC，或在安全点完整清空并重建身份；不能自然回绕。8 字节 shape 增量不是免费 padding，要计入 shape size-class 与总内存变化。

### 3.3 必须覆盖的变更点

| 事件 | 位置 | 身份处理 |
|---|---|---|
| 新 shape / clone | 5686、5781 | 分配新 ID；clone 后修改不能继承一个仍可命中的旧身份。 |
| 追加属性 | 5996 `add_shape_property` | 提交新 descriptor 后新 ID；包含无 resize 的追加。 |
| 删除属性 | 10256 `delete_property` | 失效必须覆盖释放旧值/atom 和改 descriptor 的整个操作。 |
| 删除后重排 | 5921 `compact_properties` | 新 ID，slot 编号改变。 |
| 改属性 flags | 11214 `js_update_property_flags` | 新 ID，含 writable/configurable/enumerable。 |
| data/getset/varref 转换 | 11294、11335、11369 | 新 ID，不能只改公共 flags helper。 |
| autoinit 实例化 | 9090 `JS_AutoInitProperty` | 整段不可缓存，完成或失败后新 ID。 |
| 改 prototype | 8682、8749 | 新 ID；后续原型 IC 还需检查链上的形状。 |
| fast array 变普通数组 | 10209 | 首版 class gate 排除，未来扩展前审计；转换会批量添加属性。 |
| shape/hash 搬迁、props realloc | 5847 | 仅地址或容量改变且 slot/flags/prototype 不变时可保留 ID；永远从当前对象读取 props 指针。 |
| 切换为已有共享 shape | 10186 | 使用目标 shape 的 ID，不修改其 ID。 |
| 普通字段值替换 | 2932 `set_value` | 不需要布局 ID 变化；命中路径每次读取当前值。 |
| preventExtensions | 9880 附近 | 首版“已有 own 属性”读写不受影响；以后属性新增 IC 必须另 guard extensible。 |

**只在 `js_shape_prepare_update` 里加一次 bump 不足够。**一是追加绕过它；二是释放属性值、调用 autoinit 等可能重入。若开始时 bump，重入代码可能按中间布局填入新缓存，外层随后继续修改，仍会留下错误 entry。

最低可审查的协议是“结构 mutation transaction”：变更对象设 `ic_mutating`，命中和填充均拒绝它；慢路径完成/失败后给当前 shape 分配新 ID，再清标记。嵌套变更必须继承外层的不可缓存状态，可由局部 mutation token 保存旧 bit，最外层退出才清除。所有提前 return/OOM 都走统一清理。对象切换 shape 时 bit 属于对象，仍有效。首版可更保守地用 runtime mutation-depth 全局禁用 IC 来验证正确性，再把 guard 局部化；不要一开始省掉这条协议。若实际引擎某些 mutation 中间状态本就不支持重入，也必须保持不比原实现更强的假设。

### 3.4 代码与字节码兼容

最小正确性原型先使用 PC 侧表，接受映射开销，验证布局命中与变更协议；正式 quickening 再用等长执行副本：`get_field` 的 4 字节 atom operand 在私有执行副本中换成 site index，canonical bytecode 保留 atom。新增私有执行 opcode 的数量必须检查 256 项空间，不直接假设有足够空位。

分支偏移不变仍不代表 PC 兼容已完成。`sf->cur_pc`、backtrace、generator resume、异常 catch PC 都以 canonical bytecode 为基础；helper 边界统一转换 `canonical_base + (exec_pc - exec_base)`，入口恢复时反向转换。`free_bytecode_atoms` 和 `JS_WriteFunctionBytecode` 只读 canonical buffer；执行副本中的 site index 绝不能被当 atom 释放或序列化。`JS_ReadFunctionTag:39770` 与编译创建 `37119` 都要初始化空 plan；`free_function_bytecode:37223` 释放 plan。

另一个完整而不同的选择是后续 quickening 原型采用的方案：在函数创建/读入结束、尚无任何 active frame 时建立执行副本，使 `b->byte_code_buf`、全部运行 PC 和保存 PC 始终共用执行基址；canonical 指针只供写出和 atom 释放。这避免运行中切换基址和各边界转换，但放弃按执行热度延迟创建，增加冷代码扫描及驻留。两种 PC 协议不能混用。若将来恢复运行中 tier-up，必须明确旧 frame 的代码版本/PC 身份及退出后回收，不能直接覆盖 b->byte_code_buf。

保持序列化格式 `BC_VERSION 29`（38567）不变，是通过 canonical bytecode + 私有计划实现的；不是保证旧可执行程序能理解新持久化 opcode。若选择更改公开字节码，则必须升版本并重建 `tjsc` 生成的 bundles。

### 3.5 第二阶段才扩展原型与调用

原型命中先只做一层或两层链：每层从当前对象 `shape->proto` 取下一对象，检查 layout_id、class、exotic/mutating；最终从当前 holder 的 props 读值。不缓存 raw holder 指针。receiver ID 只保护 prototype **身份**，不能保护 prototype 自身属性被修改。

缓存 method slot 不等于缓存 method 值，后者还需 value/version guard。call-site feedback 可记录“普通 bytecode function / C function / bound / proxy”及目标稳定性；调用时必须保留 this、realm、closure var_refs、异常和 interrupts。先不要新增快捷调用 ABI。

### 3.6 验证与拒绝条件

先测每子项：own/prototype/miss/exotic 占比、每 site 形状数量和熵、MONO/POLY/MEGA 动态命中率、布局修改次数、mutation 重入、feedback 字节数、shape bytes 增量。再分开测“版本字段与维护但不开 IC”和“开启 IC”，暴露框架税。

设 eligible 访问实际时间份额为 `f`，命中路径相对节省为 `e`，则忽略框架税的时间节省上限约 `f*e`。单态比例高但字段访问本身时间很小，依然不成立。以现有 slot hint 作为新增路线的比较基线。

拒绝/回退条件：反馈查表加 guard 的成本超过被移除的查找；大量 site MEGA；shape ID 膨胀/clone 或 mutation 协议成本抵消收益；warmup 和内存回归显著；无法把全部 mutation 路径纳入一致协议。测试必须覆盖 delete/重加/compact、descriptor 类型转换、Object.freeze、原型变更、autoinit、重入 finalizer、GC+地址复用、多 realm，以及 bytecode 写入/重新加载。

## 4. 路线 B：有所有权约束的热区值流 IR

这是最值得争取“大于局部微调”的路线，但应在 opcode/值流诊断后进入。已有两指令融合只消掉 receiver 一次 dup/free 和一次 dispatch；这里的目标是基本块中所有局部 load、栈 shuffle、结果转移的重复工作，不是继续堆 opcode 模式。

### 4.1 不先重写整个 VM

保留 canonical stack bytecode 作为 Tier 0 和通用回退。热函数产生 `JSExecPlan`，对热基本块模拟 operand stack、构建紧凑三地址操作。局部/参数/常量引用在 IR 构建时成为 operand，不产生运行时 `get_loc/get_arg/dup/swap` 操作；运算直接写虚拟结果槽。首版是可移植 C executor，无可执行内存、无架构专属汇编。

可复用 `compute_stack_size:36787` 的 CFG/stack-height/catch 边界知识，但当前表在编译后释放；需保留必要信息或在加载后验证并重建。`resolve_labels:35817` 已有 peephole 和短 opcode，不能误称当前没有字节码优化。动态 eval、with、captured locals、mapped arguments、generator/async 先整函数排除；字节码缺少安全证明元信息时保守回退。

### 4.2 首版所有权模型

IR 编译时每个值记录：`Immediate`、`Borrow(root)`、`Owned`。不是把额外 tag 塞进公开 `JSValue`，也不是每条 IR 都运行一套复杂所有权状态机。分析阶段将必要的 dup/move/free 编成显式动作。

* canonical locals/args 保持原有 counted ownership，不挪地址；`get_loc` 在安全区内只引用该 owner。
* 自有普通字段读若 receiver 最终扎根于未改动的 local/arg，且期间无写入、释放、分配、回调，可以借用该字段值；对象图中的实际字段引用使它活着。
* 两个 Borrow 共享 root 时不用增加 RC。结果要进入 canonical stack、heap、外部 C API、返回值或可能重入的 helper 前，按原有契约物化成 Owned。
* Owned 的真正转移可以 move；“看到最后一次读取”不等于可以提前释放一个 local。JS finalizer、WeakRef、内存限制和闭包可能观察存活期，首版不改变 canonical local 的销毁时机。

例子：`if (node.left.type === expected) ...` 的安全命中路径，在当前 VM 中会依次创建/消费 receiver 和中间字段的 counted 栈值。区域 IR 可以直接执行 `GETOWN_BORROW(left, node, ic0)`、`GETOWN_BORROW(type, left, ic1)`、受限类型的 compare+branch；local load 与中间栈变换消失，只在离开区域时产生实际需要的 owned 输出。是否值得做，取决于真实热区长度和类型分布。

### 4.3 安全区域的最小边界

首版只接受 **无重入、无分配、无 heap/local 写入的纯区域**，入口 operand stack 为空，结尾为 primitive branch 或物化少量结果。允许受 guard 的普通 own-data 读、int/bool/null/undefined 运算和比较；浮点按完整语义实现后加入，字符串仅在有明确纯 helper 时加入，rope flatten/ToPrimitive/getter/Proxy 全部失败退出。

任何 guard 失败时都可以从区域起始 canonical PC 重跑，因为没有提交可见副作用、没有释放 owner，也没有修改 locals。这是第一阶段最小 deopt：不需要一开始构造完整异常恢复机制。设置一次跳过该区域的状态，防止 fallback 又进入同一失败区域形成循环；连续失败退化为 Tier 0。

第一阶段不能只挑出 `get_loc;get_field` 当成功标准。至少应验证多级字段、字段+比较+条件跳转、两个 local 输入的算术/比较等跨 4–12 条原始指令的区域，分别报告覆盖动态原始指令数、实际 IR 操作数和退出率。

第二阶段才允许写局部、owned 结果和跨安全点执行：每个可能抛异常/重入的指令必须有 canonical PC、stack height、逻辑栈值位置及 ownership 的恢复映射；borrow 在边界前物化，locals 必须对闭包/eval 可见，执行原 helper，结果重新进入 IR。guard 失败恢复的是精确指令之前的状态，不得重放已经发生的写入或调用。

### 4.4 RC、GC、异常、调用的不变量

1. **不把 JS_FreeValue 当纯操作。**[set_value:2932](/Volumes/zhitai/workspace/lite-engine/txiki.js/deps/quickjs/quickjs.c:2932) 特意先 store 后 free，注释指出释放旧值可重分配对象数据。borrow 不能跨过可能降到零的释放；不能缓存 props 指针后调用 free 再继续用它。
2. **RC cycle GC 的根机制不能凭空替换。**运行中栈值的计数体现外部根；未计数的临时 borrow 只能依附一个仍有计数、仍稳定的 owner。首版区域不分配也不触发 GC，owner 不变；若未来允许 GC，必须把 borrowed live values 物化，或增加经过证明的 root map 协议。
3. **C API 仍用 16 字节 JSValue 和原有消费契约。**`JSValueConst` 默认只是类型别名，不能靠 C 编译器替我们证明借用，见 quickjs.h:203–222。调用前按契约建立 owned argv，返回后按原时机释放。
4. **异常清理仍然可信。**当前 `exception:20937` 会按物理栈逐值释放并扫描 CATCH_OFFSET；第二阶段必须先恢复该格式，再进入现有异常处理。不能让旧清理器看见未计数 borrow 或残留 Owned 副本。
5. **闭包地址稳定。**`JSVarRef.pvalue` 指向局部/参数槽，见 17510–17577；在做寄存器分配或栈移动前必须解决所有这些 interior pointers。首版不搬 canonical locals。
6. **中断和回边不能消失。**不能把包含循环的区域当无限无安全点纯计算。每个原有 loop backedge 的 poll 语义至少保留；首版只处理单基本块，不跨回边长期持有 borrow。
7. **数值语义不缩水。**int 溢出、-0、NaN、混合 int/double、字符串 +、BigInt/Number 混算等要 guard 或完整处理；不允许以 C 有符号溢出 UB 获得成绩。

### 4.5 为什么这比直接改寄存器字节码可控

寄存器 VM 的虚拟寄存器仍可能落在内存，不能承诺“进入 CPU 寄存器”。收益来自删除不必要的显式 stack instructions 和 ownership 往返，并给 C 编译器或后续代码生成器更长的可见值流。IR 若膨胀成 24/32 字节一条，执行 fetch 的代价可能超过被删的 1–3 字节短 opcode；用 8/12 字节可编码操作、分离冷 metadata，并对热区覆盖设门槛。

第二阶段可以选择“热区解释执行”与“完整函数 register lowering”作比较。只有前者证明足够覆盖且 dispatcher 自身仍明显占比，才值得维护完整函数寄存器 frame、所有异常/生成器状态和新 call ABI。

### 4.6 分阶段验证与拒绝

**B0，诊断，不改执行。**计数 opcode family、相邻/短序列、动态 get_loc/get_arg 次数、其引用类型比例、纯区域长度、借用值 crossing-safepoint 比例、可消除 dup/free 对数。按子项和固定调用次数输出；诊断二进制不拿来计分。

**B1，纯区域。**只在 stack-empty 基本块构建计划，沿用 A 的 own-data guards；加入 guarded numeric/compare branch；失败起点重跑。原始代码/解释器保留，计划分配失败安静回退。性能对照至少包含：框架开启但所有区域禁用、真正区域执行、区域执行但强制保留原 RC 操作（诊断差分），避免把代码布局偶然改善当架构收益。

**B2，显式物化安全点。**先支持区域末端写一个 uncaptured local，完整保持 store/free 顺序；再支持有限 helper，逐条建立恢复图。测试覆盖异常、重入、生命周期和一百次反复 deopt，不只测数值正确。

**B3，可选函数级 register executor。**只有 B1/B2 的实测覆盖和收益足够才进入。与 A 分开计分，再测试组合，因为两者会优化同一属性读，收益不能相加。

拒绝条件：可行区域大多只有 1–2 个有效操作；borrow 频繁在 helper 边界物化；guard miss/多态频繁；plan 构建、内存与 instruction footprint 抵消吞吐；减少的 dispatch 很多但时间几乎没变；需要在首版同时重写闭包、async、异常和 GC 才能覆盖足够工作。

## 5. 路线 C：普通对象 literal 的布局预规划

这是第三优先级，先统计覆盖再实现。它针对 AST 节点一类对象的“创建过程”，不是已经测试的数组 props 合并分配。

### 5.1 现状与目标

[js_parse_object_literal:25264](/Volumes/zhitai/workspace/lite-engine/txiki.js/deps/quickjs/quickjs.c:25264) 先发 `OP_object`，之后逐项 `OP_define_field`；源码明确留有 initial-length TODO。`OP_define_field:19749` 每次走 `JS_DefinePropertyValue`，进入完整 descriptor 语义；`add_property` 逐步找/克隆 shape、扩容 props。即使最终对象形状高度重复，仍可能在构造时付多次中间状态成本。

为编译器能够证明安全的 literal 构建 `ObjectRecipe{atoms[], flags[], count, final_shape}`。首版限定普通、唯一、静态名字的数据属性，排除 computed keys、spread、getter/setter、method/home-object、`__proto__`、重复 key。保持表达式求值顺序。

### 5.2 最小执行方案

只在新对象确实尚未逃逸的 literal 初始化区域：按 final shape 一次分配 props，全部槽先设 JS_UNDEFINED，然后每个原 define_field 点按已知 slot 接管值。这个对象在 JS 可获得它前始终留在内部 operand stack；排除 method/home-object 等可以建立外部通路的语法。初始化表达式仍可调用或抛错，但调用不能访问这个尚未暴露的普通对象。

最后一次字段完成后，对外就是普通对象，无特殊生命周期。异常时现有 free_object 能安全遍历已初始化和 undefined 槽；任何 GC 时也只能看到有效值。recipe shape 的强引用是有意的常量布局模板，需要在 `mark_children(FUNCTION_BYTECODE)` 增加边，在 free_function_bytecode 中释放；这与 IC 不应强持 shape 的理由不同。

更保守的 C1 可先只预分配 props 容量、仍按原顺序增长可见 shape，验证扩容成本；C2 再建立 final shape + 直接字段初始化。不要把容量预留结果当作消除了 shape 转换。构造器的 `this.x=...` 不享受同样的“不逃逸”证明，不能自动套用 literal 方案；构造器优化需另做 escape/副作用分析。

通过 JSExecPlan 记录 recipe，canonical 字节码不变；解释执行副本或 region 负责对象创建点和 init slot 的映射。若新增持久化 opcode，则明确升级 BC_VERSION 并重建 bundles。最初无需把 props 内嵌到对象，因为主要实验问题是“能否删除整个逐字段创建流程”；内存布局合并可作为后续独立变量。

### 5.3 边界与失效

recipe 绑定 bytecode realm 的普通 Object prototype，proto identity/layout 必须按创建语义正确取得；不能把某个 runtime 的 shape pointer 序列化到另一 runtime。修改属性 flags、delete、后续动态新增仍回普通对象路径。

预分配会改变峰值内存和 OOM 触发位置，初始化表达式执行中失败时还必须释放所有已接管值。先只处理没有 spread/重复 key 的精确 count，避免盲目高估容量。首版必须证明对象未逃逸，不能以“这是 AST 应该没事”替代证明。

### 5.4 频率与拒绝条件

计数：按 allocation PC 的创建次数、字段数分布、唯一静态 literal 比例、构造器与 literal 占比、每对象 resize/clone 次数和搬运字节、最终 shape 重复程度、生命周期分布。记录构造期间和返回后的峰值内存，统计每子项复用 recipe 的次数。

sample 中 add_property inclusive 3.74% 与 NewObjectFromShape inclusive 1.88% 是该窗口的线索，且会重叠；不能承诺 C 路线拿走这二者全部，更不能外推到整套。PGO 计数证实 resize/clone 高频，仍需按子项 self-time 与 eligible 比例定位。

拒绝条件：大部分节点由可逃逸构造器/动态字段创建，安全 literal 覆盖低；中间 shape 已被充分共享、扩容实际耗时小；recipe 强持 shape/atoms 使长期内存明显增大；预分配加大 cache footprint；新增执行映射成本压过创建节省。潜在收益更可能集中在 parser/transformer，source-map/正则/字符串主导子项可能接近零或受内存影响退化。

## 6. RC、短命 AST 与长活对象：现在该做什么

QuickJS 的“年轻对象”不是没人回收：无环短命树往往在 RC 归零时已经回收。改为 generational tracing 可能省引用写入，却新增 root maps、write barriers、老→年轻 remembered set、C API 外部根，以及 custom class gc_mark、WeakMap/WeakRef/FinalizationRegistry 处理。收益和成本不可以只按“AST 短命”判断。

目前 cycle collector 是全图 trial deletion：`gc_decref` 减所有内部边、`gc_scan` 从剩余外部引用恢复、`gc_free_cycles` 删除循环。阈值在 `js_trigger_gc:1681` 由当前分配量增长 50% 重设。sample 的 inclusive 20/8522 不支持先把它改成分代；降低 0.235% 窗口成本不可能单独产生大幅加速。但这也不能证明其他子项没有 GC 问题。

先补生命周期证据：每对象 class/recipe/site 的 birth allocation serial、释放年龄、归零释放还是 cycle 释放、图边数、GC 扫描/实际回收比、每轮存活 bytes、长活字节码和 prototype 比例。采样记录即可，不要给每个对象永久加大 header 来测量。

若后续看到周期 GC 占比明显且多数对象长活，才评估 cycle candidates/分代 trial deletion；保持所有引用计数准确，只缩小周期扫描范围，也需要对新边和变更边建立完整协议。若看到 RC 写入/临时 dup-free 主导，则 B 的区域所有权消除是更小的第一步。若看到实际析构主导，则研究批量释放或布局改善，但保持 finalizer 顺序和重入；单纯延后 zero-ref 释放可能提高峰值内存并改变宿主资源释放时间。

不要将某轮 benchmark 的 AST 放进 arena 后直接整区清空：结果可逃逸到缓存/closure，工具常复用节点，JavaScript 没有“本轮作用域全部死亡”的契约。即使给特定调用加 region allocator，也必须有完整逃逸提升与外部引用管理；当前不是最低风险路线。

## 7. 16 字节值、C API、跨平台与可选 JIT

### 7.1 先减少搬运次数，再讨论缩值

现有 `JS_NAN_BOXING` 明确为 32 位构建启用，quickjs.h:174–177；其 tag/pointer 编码不是可直接在 arm64 打开的通用开关。64 位新编码需要处理地址有效位、NaN payload、-0、pointer provenance、C++ 宏/构造、调试器及平台 ABI。公开 JSValue 改变会影响所有 native modules 的参数、返回值和数组布局。

保持外部 16 字节、内部改 8 字节也不是免费兼容：每次 C API/argv/JSVarRef 边界要装箱/拆箱，get/set helper 要适配两种表示；并且 props union 仍为 16 字节。可移植的第一步是 B 删除不必要的值传输；后续只对经过证明的数字热区用 unboxed int/double 寄存器，边界物化普通 JSValue。

### 7.2 一条可逐层停下来的执行体系

| 层 | 内容 | 跨平台策略 |
|---|---|---|
| Tier 0 | 当前 canonical stack interpreter | 全平台总是可用，冷代码与全部复杂语义的真值路径。 |
| Tier 0.5 | A 的 feedback/IC/等长 quickening | 纯 C，独立可开关；不依赖 PGO。 |
| Tier 1 | B 的 value-flow region executor | 纯 C，可单独形成产品结果；反馈/安全点/ownership maps 与以后 JIT 共用。 |
| 可选 baseline JIT | 从已验证 IR 输出少量机器码或模板片段，沿用同一慢路径 | 只在有后端的平台启用，其他平台继续 Tier 1；不改变源码/字节码/API 兼容基线。 |
| 后续优化层 | 热循环 unbox、有限内联、guard hoisting | 必须在真实反馈与精确恢复图成熟后做，不在首轮承担。 |

baseline JIT 若只是把每条 stack opcode 翻成一次 C helper 调用，可能仅减少 dispatch，却保留 RC/值搬运并新增 call ABI 开销。真正可复用的资产是 A 的 guards、B 的值流与恢复图，不是可执行内存分配本身。代码生成的 invalidation 必须能在 helper 重入后检测 layout 变化；old code 在活跃栈帧退出前不可释放，异常和 source-PC 仍映射 canonical bytecode。平台后端还需 W^X、指令 cache 同步、栈对齐、寄存器保存等实现与验证，不能靠关闭平台约束换性能。

目前递归 `JS_CallInternal` 与动态 alloca 也值得计数，但直接改为 VM trampoline 不是本报告前三路线：内部 argv 已有零拷贝快路，新的可移动大栈会使 JSVarRef 的 interior pointers 失效。若调用/frame 成为明确瓶颈，可先使用不可移动 stack chunks 建 shadow frames，只让普通 bytecode→bytecode 调用在 C 循环内切 frame，C callback/async 保留桥接；仍需重做异常 unwind、stack limits、realm 和闭包关闭。不要把这项与 B 首版绑在一起。

## 8. 必须先取得的测量与决策规则

下一轮不是再猜十个快路径，而是建立 `QJS_EXEC_STATS` 诊断模式。按 WTB 子项边界 reset/snapshot；若不能修改 WTB，则分项进程运行，外层不计分。计数模式与正式计时模式严格分离，不能拿 instrumentation 降速后的频次当正式耗时比例。

| 类别 | 最少计数 | 用来回答的问题 |
|---|---|---|
| 执行 | 每 opcode family 动态次数、原始 bytecode 数、分支/回边/调用数 | dispatch 有多少、哪些 load/shuffle 可静态删除？ |
| 值流 | 局部/参数读取的 tag 分布、RC dup/free、zero-ref 比例、可配对消除数 | 在已有 helper/fusion 之后，还剩多少所有权往返？ |
| 属性 | own/proto/missing/exotic/descriptor-type、site 多态和复用 | IC/区域的覆盖能否超过维护成本？ |
| 对象 | allocation site、最终字段数、增长次数、shape clone/lookup/搬运 bytes | C 路线能消掉多少真实构造工作？ |
| 生命周期 | 分配年龄、归零/循环回收、峰值活集、GC 扫描边/回收边 | 应优化临时 RC、析构，还是周期 GC？ |
| 分层 | plan 构建时间/大小、执行次数、热区长度、miss/deopt、物化次数 | 是否达到热区摊销要求？ |

采样还需要按子项覆盖稳态与冷启动，或至少给完整运行加 phase 标记；不要只凭第一段 10 秒。确切拆分 JS_CallInternal 内部，可在诊断版本做 opcode 类别计数，再用源码/PC 对照样本和受控候选差分；没有可靠 PMU 时，不必声称知道 cache/branch miss。

Amdahl 计算采用时间份额而不是调用数。若某子项中 eligible 工作占 `f`，候选将该部分变为原来的 `r`，新增加成本占原时间 `h`，则 `speedup = 1 / ((1-f) + f*r + h)`。例如仅作为算例，`f=0.25,r=0.5,h=0.02` 得约 1.117 倍；这不是对本引擎的预测。要达到 20% 吞吐增益，净时间必须减少至少 16.67%。即使 10 秒窗口的解释器 62.34% 可代表目标工作，若仅把其中 10% 消掉，吞吐也只有约 6.65% 上限，而不是 62%。

17 项 geometric mean 的组合为 `exp(mean(log(speedup_i)))`。允许少量小退化是合理目标，但不能用某一项大涨遮蔽重复出现的重大倒退；单项、总分、冷启动、最大 RSS 和代码/plan 大小一起记录。各路线先单独与当前最优基线比较，再测组合，明确同一成本不能重复认领。

建议执行顺序：**B0 诊断与 A 的布局协议/反馈采集 → A own-data IC → B1 纯区域 → 根据覆盖选择 B2 或 C1/C2 → 最后才决定可选 JIT、完整 register VM 或 GC 改造。**每一步都是可单独拒绝的实验。若诊断显示 eligibility 太低，直接停止该路线；不以已经投入的工程量作为继续扩大范围的理由。

后续 JIT/Java 借鉴工作的可执行起点就是 `JSExecPlan + layout_id + Borrow/Owned + canonical-PC map`：先实现纯 C、无副作用区域及可证明恢复，再选择是否增加机器码后端。这样能保留全平台解释器，并把性能收益归因到具体删除的工作，而非某个架构名称。
