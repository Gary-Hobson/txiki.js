# 从 HotSpot / Graal 借鉴到可移植 QuickJS 解释器

2026-11-10。目标仍是 Release + ThinLTO、无 PGO、无 inspector 的 Web Tooling Benchmark；不生成机器码，不要求特定 CPU。本文承接 [整机架构分析](/Volumes/zhitai/workspace/lite-engine/txiki.js/benchmarks/quickjs-architecture-analysis.md)，记录官方来源、迁移边界和一个已经实现的独立实验。

结论：可迁移的核心是 **按执行点形成假设、消除重复的值表示、保留精确回退**。但实现方式必须经实测筛选：首版借用谓词区域通过正确性测试，六对 WTB 吞吐中位下降 **7.13%**，已经淘汰。唯一修正版改为只在真实区域入口执行额外逻辑，独立六对中位 **+0.59%**、5/6 胜，尚未证明相对已保留 hint/fusion 的增量；这支持继续重视入口税，不支持宣称大幅架构突破。两版均以 borrowed 值执行多级字段读取与严格比较，成功才提交一个 boolean，不是完整的逃逸分析、对象标量替换或寄存器 VM。

## 1. 官方实现给出的设计依据

| 机制 | 官方材料/源码中实际存在的内容 | 可迁移到 QuickJS 的部分 |
|---|---|---|
| 按调用点的类型反馈 | HotSpot `Compile::call_generator` 从 caller 的 BCI 读取 call profile，识别主 receiver、单态/双态，并考虑此前 trap/recompile 次数。 | 以 bytecode PC 为单位记录 receiver layout / callable 类型，而不是全 runtime 按 atom 混在一起；多态与反复失败必须能退回通用路径。 |
| 逃逸分析和可标量替换性 | HotSpot escape.cpp 先构造连接图并传播逃逸信息，之后还单独判定 scalar_replaceable；非逃逸并不自动等于可删除分配。 | 必须区分“值没有离开区域”“对象没有逃逸”“对象身份可以被消除”三个证明。 |
| 部分逃逸 | Graal 的 PartialEscapeAnalysis 允许在热路径虚拟化对象、到逃逸路径才分配，并明确有编译时间成本。 | 先限定在热区域和少量形状；可在纯 C executor 中使用虚拟对象表，遇调用/存 heap/返回才物化，但要承担 RC 和 OOM 的恢复设计。 |
| 恢复对象和 frame | HotSpot deoptimization.cpp 调用 realloc_objects 和 reassign_fields；Truffle 文档说明优化后的 VirtualFrame 在回退时需恢复。 | metadata、live-value map、ownership map 是核心功能，不是最后补的调试附件。 |
| 循环提升 | HotSpot loopPredicate.cpp 的 Invariance 同时分析输入和控制依赖；Graal 区分循环不变代码和有边界的展开。 | 只有在别名、写入、回调与零次循环语义均成立时，才把检查或读取移出循环。 |

来源为官方仓库和文档： [HotSpot doCall.cpp](https://github.com/openjdk/jdk/blob/master/src/hotspot/share/opto/doCall.cpp)、[HotSpot escape.cpp](https://github.com/openjdk/jdk/blob/master/src/hotspot/share/opto/escape.cpp)、[Graal CEOptimization.java](https://github.com/oracle/graal/blob/master/compiler/src/jdk.graal.compiler/src/jdk/graal/compiler/core/phases/CEOptimization.java)、[HotSpot deoptimization.cpp](https://github.com/openjdk/jdk/blob/master/src/hotspot/share/runtime/deoptimization.cpp)、[HotSpot loopPredicate.cpp](https://github.com/openjdk/jdk/blob/master/src/hotspot/share/opto/loopPredicate.cpp)、[Truffle Partial Evaluation](https://www.graalvm.org/dev/graalvm-as-a-platform/language-implementation-framework/PartialEvaluation/)。本次查看的是 2026-11-10 可见的 master/dev 页面，不将其当作某个已发布 JDK 的精确功能清单。

[OpenJDK 的 EA/SRA 历史分析](https://cr.openjdk.org/~cslucas/escape-analysis/EscapeAnalysis.html) 有助于理解条件逃逸、未内联调用和控制流合并的困难。它是较早时期的状态报告，本文不把其中“当前不支持”的描述直接套到最新 HotSpot。可复用的经验是：局部非逃逸性不够，整个值流和恢复路径都要成立。

## 2. 类型反馈与去虚拟化：哪里相同，哪里不同

HotSpot 的 receiver class 与 QuickJS 的 `JSObject.class_id` 不等价。QuickJS 中大量普通 JS 对象都属于 `JS_CLASS_OBJECT`，字段集合、descriptor、prototype 和 callable 值仍可以变化；class guard 不能代替 layout guard。对象 `obj.method` 的 shape 稳定，也不意味着 method 值不变。

因此先迁移“按 PC 反馈”的组织方式：每个访问点统计少量 layout IDs 及命中/失败，普通 own-data 才走 slot load；方法调用必须读出当前 callable 后再 guard 类型/目标，并保留 this、realm 和 closure var_refs。先不尝试将 JS 方法内联进 C 解释器。真正的去虚拟化需要可靠失效协议，详见主报告 shape mutation transaction。

当前 predicate 原型没有做 shape IC：每次执行仍验证 class、exotic 和当前 descriptor，并通过现有 find_own_property 读取。它利用的是 canonical bytecode 的稳定结构，而不是推测对象形状不变。这样的实验把“值流/所有权消除”与“形状 IC”分开，便于判断实际收益来源。

可实施的后续步骤是给 decoded recipe 的每次字段读附上 A 路线的 own-data IC；成功后省字段查询，失败后原区域完整回退。不要同时引入 method inlining、原型链缓存和全函数 register conversion，否则无法辨别收益或问题。

## 3. 逃逸分析、scalar replacement 与 RC 所有权

Java 优化中对象分配可以在证明身份不可观察后消失，GC 以精确根和安全点维护活值。QuickJS 使用 RC：局部、栈、属性等持有计数，zero-ref 释放可以调用 native finalizer，cycle collector 通过计数和内部边识别外部根。所以不能把某个对象“放到 C 栈上”就宣称完成 Java 式逃逸优化。

可迁移的两层目标应分开：

1. **删除临时值表示。**已有对象始终真实存在；区域只借用由 canonical local/arg 和对象字段持有的值，不为每个短暂栈副本增减 RC。当前原型属于这一层。根稳定、没有释放/回调/分配，所有权证明简单。
2. **删除对象本身。**例如局部 `{x:a,y:b}` 只被普通读取后丢弃，才可能用虚拟字段槽替代对象。至少要排除身份比较、WeakRef/WeakMap、prototype/reflection、setter/getter、Proxy、native callback、闭包捕获和 heap store；若走不支持的路径，要物化一个具有正确 prototype、字段顺序、descriptor 和身份共享关系的真实对象。

第二层的最低原型应限制在没有 escape 的单基本块：fresh 普通 literal、固定字段、没有调用和别名，结果只消费其字段。物化点出现前，虚拟字段里的引用值仍必须有真实 counted owners，或独立 owned slots。遇异常/OOM 时不能双重释放，也不能将已经执行过的外部副作用重跑。这个工作有合理潜力，但现有 WTB 还没有“安全非逃逸对象占比”的统计，不应取代本轮更小的值流实验。

主报告的 ObjectRecipe 则是第三种优化：对象仍分配，只预规划最终 shape/props，减少逐字段构造成本；它既不是栈分配，也不是 scalar replacement。三者的成本与正确性边界不能混在一个百分数里。

## 4. loop hoisting：先提升检查，后考虑数据

在纯解释器中也可以实现受 guard 的 loop region。候选是：对一个未改写的普通对象，循环多次读取同一普通字段，区域内部没有可能修改对象的调用、store 或别名。循环入口 guard 布局；循环内可以复用 slot 编号，仍读取当前值。这比直接缓存字段值假设更弱。

提升字段**值**需要证明循环中没有经任何别名修改它。AST 遍历经常通过 visitor/callback 回调，回调可能修改节点，因而“输入节点看似只读”不够。数组 length、typed array backing pointer 也不能跨可 detach/resize 的操作缓存。

提升读取还会改变零次循环的行为：原来未执行的 getter/异常不能因提升而发生。只有受 guard 的普通数据读、且对象有效性先被证明，或使用保留首次迭代检查的 loop versioning，才能处理这个问题。poll/interrupt 仍按原引擎语义保留；宿主回调回来后重新验证必要状态。不能为了少一次 poll 而引入不可中断循环。

本轮原型刻意不消费 `if_false8/if_true8/goto*`。比较后的条件跳转及 `js_poll_interrupts` 仍执行原 handler，用更小的正确性边界先验证值流是否值得继续。

## 5. 已实现的原型：borrow-predicate

源文件：[borrow-predicate-transform.py](/Volumes/zhitai/workspace/lite-engine/txiki.js/build/lto-benchmark/quickjs-hotpaths/borrow-predicate-transform.py)。测试：[borrow-predicate-correctness.js](/Volumes/zhitai/workspace/lite-engine/txiki.js/build/lto-benchmark/quickjs-hotpaths/borrow-predicate-correctness.js)。它只转换隔离构建源码，没有修改 `deps/quickjs/quickjs.c`。

### 5.1 数据结构与计划构建

`JSFunctionBytecode` 增加 `borrow_predicates` 指针和饱和预热计数。普通 bytecode function 第 32 次调用时做两遍只读扫描：第一遍计数可执行 recipe，第二遍填入一块单独分配的 map+recipes。没有匹配 recipe 就不分配；函数 bytecode 大于 65535 字节不处理。阈值 32 只是原型的摊销选择，尚未证明最优。

map 是 `uint16_t[byte_code_len]`，索引是第一个 local/arg load 后面的 PC；0 表示未编入区域，其余指向 8 字节 recipe。recipe 记录右操作数入口、比较 PC、左右字段级数、右 producer 大小和 eq/neq。两侧字段链各最多四级，整个模式至少含一个字段读。

这是有意选择的最小区域计划，而非每次 get_loc 都扫描字节码。代价也清楚：有 recipe 的函数约需 `2*byte_code_len + 8*recipe_count + header/alignment` 字节，加所有 bytecode 的新增结构字段；低覆盖的大函数可能很不划算。计划分配使用 runtime allocator，不创建 JS exception，失败就继续原解释器；free_function_bytecode 释放整块计划。

### 5.2 认可的区域语法

```text
local/arg
  get_field*        // 左侧 0..4 级
local/arg/constant
  get_field*        // 右侧 0..4 级
strict_eq | strict_neq
```

这是一段完整 readonly expression：运行时借用左根和右根，逐字段验证并读取，最后调用非消费型 `js_strict_eq2` 返回 boolean。成功才改变 PC 到比较之后；handler 只压入一个 boolean，其他临时栈值和 RC 往返消失。典型例子是 `node.left.type === other.right.type`、`node.type === "Identifier"`。

它不会把原有 `get_loc0_loc1` 特殊双 load 解析成两个普通 load；不支持的 opcode 或超过四级字段链直接不建 recipe。可以从已有非空 operand stack 进入，因为区域只取自己产生的两个操作数，既不读取也不改写之前的栈内容；区域中不含 branch/call/store/异常 handler 操作。

### 5.3 实际 guard 与 fallback

* 每级 receiver 必须是 `JS_CLASS_OBJECT` 且非 exotic，属性必须当前存在并是普通 data descriptor。
* getter、Proxy、prototype lookup、autoinit、varref、缺失属性、primitive receiver 都失败退出，不执行用户代码。
* 比较只接收 int/double/bool/null/undefined/object/symbol/平坦 string；rope、BigInt 和内部 sentinel 保守退出。
* `push_atom_value` 的整数 atom 会转换成十进制字符串，可能分配，故退出；普通 string/symbol atom 借用字节码本来拥有的 atom 引用。
* `js_string_eq` 已沿源码核对为长度检查和纯字符比较；`js_strict_eq2` 对这里允许的类型没有分配、JS 回调或 interrupt poll。rope/BigInt 实际源码也有专门比较，但原型暂不扩大证明面。
* guard 失败时 PC、SP、locals、args、heap、RC 均未改；重新执行原 load。已经完成的只读数据字段查看没有可见副作用，故不重复 getter/Proxy。
* 组合时 fallback 调用已有 `js_load_local_or_field`；独立时 fallback 是 `js_dup`。应用顺序必须是 hint/fusion 在前、borrow 在后。

canonical bytecode、atom 引用、分支偏移、source-PC、C API 和 JSValue 表示都未改变。recipe 只存字节偏移和小整数，没有 shape/object/JSValue 所有权，因此无需新增 GC mark 边。当前不缓存 shape 也不缓存 props pointer，属性被修改后下一次必重新验证。

### 5.4 正确性证明的关键点

连续字段链的中间对象可能只由前一个字段持有，但最初 local/arg 始终持有根，区域没有任何修改、free 或 callback，整条强引用路径不会消失。比较结果是立即数，不需要维持中间 borrow 到区域外。

若右侧 getter 会改变左侧对象，guard 在调用 getter 前失败，fallback 按原顺序执行左读、右 getter、比较，保留“左值是 getter 之前快照”的语义。测试包含这一情况，并检查 getter/Proxy 只调用一次。

这段执行不跨 poll；比较完成后原有 branch poll 仍能中断。若未来加入可抛异常 helper 或 store，此证明即失效，必须改为主报告描述的精确物化/恢复，而不是继续往当前 helper 塞特例。

## 6. 现有证据与验证进度

新增完整运行 opcode 诊断：[opcode-profile.md](/Volumes/zhitai/workspace/lite-engine/txiki.js/benchmarks/results/opcode-profile.md)。其中总 dispatch 4.706B，strict_eq 182.816M，get_field 349.238M；连续 pair `strict_eq→if_false8` 113.409M、`get_field→get_field` 42.664M、`get_field→get_loc8` 42.076M。它支持优先探测字段谓词区域，但不同 pair 不能拼成未经测量的长序列覆盖率，且计数含 runtime 启动并可能跨调用。

第一版完成后的记录：

| 项目 | 状态 |
|---|---|
| baseline、hint、fusion、hint+fusion 四种输入的 transform 锚点检查 | 已通过纯 Python 检查，均产生 11 个 load 入口替换 |
| 类型/所有权/分支与源码审查 | 已完成初审，rope/BigInt 等保守回退 |
| 隔离构建、专项/已有 property/fusion 测试 | 测试代理确认编译成功；基线与候选均通过三组正确性测试 |
| baseline 对照预热与六对 WTB | 已完成：配对总分中位 **-7.13%**、均值 **-5.88%**，1/6 胜；淘汰 |
| metadata 内存统计和动态成功/失败次数 | 当前原型尚未补齐；若进入下一版，必须增加 |
| Windows/Linux/32 位编译和 sanitizer | 尚未执行，不能把“纯 C 可移植设计”写成“已全平台验证” |

原始结果：[borrow-predicate/comparison.json](/Volumes/zhitai/workspace/lite-engine/txiki.js/build/lto-benchmark/quickjs-hotpaths/borrow-predicate/comparison.json)。六对 baseline 总分为 `[3.06, 3.18, 3.18, 3.41, 3.41, 3.39]`，候选为 `[3.12, 2.99, 2.98, 3.12, 3.12, 3.12]`。初期有其他构建干扰，后三对仍明显下降，因此不能用干扰否定淘汰结论。此结果针对该实现，不能证明所有区域执行方案都无效。

源码可确认的框架税包括：每次普通函数进入的预热检查、全部 11 类 local/arg load 的 helper 调用和 table/map 查询，以及 recipe、字段 guard、元数据和扫描。额外 helper 参数是否造成关键寄存器 spill、实际 eligible 覆盖和 guard 成功率仍未测得；把下降完全归因于某一项会超出证据。修正版只针对最明确的广泛入口税，不同时增加 shape IC、支持类型或新子系统。若修正版可接受，再对 hint+fusion 做增量比较，不能重复认领已有收益。

仍缺的动态诊断是 recipe 函数数、总 map/recipe bytes、函数执行次数、map 命中、完整成功、失败原因、成功消费原始 opcode 数、避免的 refcounted dup/free 对数。修正版已补 `JS_ComputeMemoryUsage` / `compute_bytecode_size` 的新增 allocation count 和 bytes；这些会进入现有内存报告，不等于已取得动态覆盖率。已测试第一版文件保持不变。

验收看真实吞吐及子项，不看“使用了 EA/JIT 思想”的名称。若回归或收益不稳定，记录失败；若命中少，可依据诊断缩窄准入/降低 map 税做独立变体，不能在没有证据时扩展到完整 JIT。只有完整区域确实高频且 ownership/dispatch 消除能产生稳定收益，再扩大值流 IR 或引入虚拟对象。

## 7. 不引入平台依赖的边界

原型使用已有 QuickJS 的 JSValue/atom/allocator/opcode helper 和标准 C 控制流，未使用 arm64/x86 指令、可执行页、指针压缩、NaN boxing 开关或宿主特有信号处理。字节码来自既有 parser/reader，执行计划是可丢弃的 runtime metadata。

正式保留前仍需验证 32 位 JS_NAN_BOXING、不同编译器、QJS_DISABLE_PARSER、ENABLE_DUMPS 和 sanitizer 构建；ENABLE_DUMPS 当前禁用该优化以保持逐字节码调试行为。跨平台策略是保留原解释器为完全可用的 fallback，而不是在某个平台上悄悄降低语言语义或保护措施。

可继续借鉴 Java 的主要顺序是：**可测的执行点反馈 → 可恢复的值流区域 → 受限逃逸/虚拟对象 → 经过别名证明的 loop guards/loads 提升**。当前实验在第二步建立可复核起点；后续每步仍需要独立证据和明确拒绝条件。

## 8. 唯一修正版：区域入口私有 quickening

文件：[borrow-quickening-transform.py](/Volumes/zhitai/workspace/lite-engine/txiki.js/build/lto-benchmark/quickjs-hotpaths/borrow-quickening-transform.py)。它复用第一版的区域语法、guard 和无副作用证明；普通 local/arg handlers 恢复原实现，或恢复已保留的 simple load-field fusion。只有编入 recipe 的入口承担区域执行开销。

### 8.1 执行副本与生命周期

构建时机改为 `js_create_function` 与 `JS_ReadFunctionTag` 返回前，取消第 32 次调用预热。此时尚无该函数的 active frame。找到 recipe 后一次分配 `table + aligned uint16_map + recipes + execution_copy`，复制 canonical bytecode，只把区域首个 local/arg opcode 改为私有 opcode 255；不改变任何指令大小、operand、分支偏移或 source-PC offset。

`b->byte_code_buf` 从第一次调用起就是 execution_copy。普通 frame、generator/async frame 的 `cur_pc`、catch PC 和回溯 PC 全部使用同一执行基址，运行中从不更换。原始 canonical bytes 仍在原 `JSFunctionBytecode` allocation 中；`borrow_canonical_code` 指向它。序列化复制 canonical bytes；释放函数时先恢复 canonical 指针，再释放 plan，原 atom 遍历只读 canonical。没有增加 atom/JSValue/shape 强引用，无需额外 GC mark 边。

可选 allocation 仍为唯一的 `t = js_mallocz_rt(rt, bytes)`：返回 NULL 时 plan/canonical/bytes 字段仍为零、byte_code_buf 未动，不设置 JS exception。它使用 runtime allocator，不调用会自动 throw 的 context allocator。后续必需分配仍可能受额外存活 metadata 的内存占用影响，因此低内存验证不能只检查这一次 malloc 的返回值。

### 8.2 私有 opcode 的隔离与 fallback

255 不加入公开 opcode 表，以 `_Static_assert(OP_COUNT < OP_borrow_predicate, ...)` 防止未来编号冲突。构建器扫描完整 canonical bytecode，遇 `op >= OP_COUNT` 就不建 plan；没有合法 plan 的 255 handler 进入原 invalid opcode 错误路径。私有入口还核对 canonical root 是受支持的 local/arg load、offset 合法、map entry 存在。普通序列化输入不能直接授予私有执行资格。

handler 从 canonical root 读取 initial borrow，再按原 root 指令长度设置其后 PC。完整成功才跳到比较之后；guard 失败仅执行原 root load（组合时可使用已有 simple fusion），后续原指令继续执行。私有 opcode 代替的是同一 root 指令，其他字节未改，因此跳转到区域内部仍从该位置的原始 stack 语义执行；无需新增函数入口/回边 poll，也不消费条件分支。

### 8.3 调试、统计与验证门槛

`ENABLE_DUMPS`、`JS_DEBUG_TRACE_DEBUGGER_STMT` 或 `QJS_DISABLE_BORROW_QUICKENING` 任一已定义时，prepare 直接返回。inspector 检测来自 debugger patch 添加到 quickjs.h 的公开宏，不依赖 transform 输入源码是否已经打过 debugger patch。已用临时副本验证 **quickening→debugger** 与 **debugger→quickening** 两种顺序均可应用；正式构建的前者也能看到同一 header guard。未声称该静态检查等同于完整 inspector 构建测试。

新增 plan 和 execution copy 的一块 allocation 已计入 `memory_used_count`，其完整 `borrow_plan_bytes` 计入 `js_func_size`；新增字段由 `sizeof(*b)` 计入。额外内存约为 `3*byte_code_len + 8*recipe_count + header/alignment`，且所有具有 recipe 的冷函数也会分配。取消热阈值换来稳定 PC 基址与零普通入口税，但增加编译/读入扫描和冷代码驻留，是这个修正版的明确取舍。

baseline、hint、fusion、hint+fusion 的 transform 静态检查均通过，恢复全部 11 类普通 load 入口；两个创建/读入路径均准备，warmup 字段已删除。独立代理已确认：编译和专项通过；补强后 generator/async **自身直接执行字段谓词**、try/catch/finally、源码定位在 baseline 与候选均通过；native fixture 保留 compile-only bytecode，热执行前后序列化完全一致，baseline 与 quickening 双向互读执行通过。对唯一计划分配强制返回 NULL，compile 与 read 两条路径都实际触发并成功回退。inspector 完整构建/运行仍未执行，不能把宏和 patch 顺序审查写成 inspector 已实测。

### 8.4 六对结果与本轮决策边界

原始数据：[borrow-quickening/comparison.json](/Volumes/zhitai/workspace/lite-engine/txiki.js/build/lto-benchmark/quickjs-hotpaths/borrow-quickening/comparison.json)。baseline 总分 `[3.42, 3.43, 3.41, 3.42, 3.41, 3.39]`，候选 `[3.43, 3.42, 3.43, 3.44, 3.44, 3.43]`；配对中位 **+0.586%**、均值 **+0.539%**、**5/6 胜**。单项配对中位最高 babylon **+2.57%**；typescript **-1.66%**、babel-minify **-1.43%**，espree **-0.45%**，其余接近零或小幅正向。基准总分输出只有两位小数，不能把更多小数位当测量精度。

修正版消除了首版广泛的执行入口开销后，总体从明显负值回到小幅正值。这与入口税假设相符，但改动也同时取消热阈值、改变准备时机和代码布局，因此不是逐因素因果分解。没有命中/失败与动态消除计数，仍无法将收益精确拆成 RC、栈写入和 dispatch 三部分。

这次实验未达到“大幅提升”的目标。随后针对已保留的 hint+fusion 做六对增量比较，配对中位 **-1.00%**、**0/6 胜**，typescript **-3.55%**、esprima **-2.72%**、uglify-js **-1.92%**。原始结果见 [final-quickening-combined.json](results/final-quickening-combined.json)。属性、局部字段融合、借用谓词及序列化专项通过，但正确性不能替代性能收益，因此不纳入正式 patch，停止此候选。独立收益不能与已保留优化相加。

内存也存在明确成本：1000 个不匹配函数的 native fixture 比基线多 32,032 字节，1000 个单字段比较函数多 110,032 字节；两对完整 WTB 的峰值 RSS 约低 2.1 MB，不足以证明内存改善。详见 [native 内存记录](results/borrow-quickening-native-memory.json) 和 [RSS 记录](results/borrow-quickening-rss.json)。保留该原型作为等长执行副本与精确回退的实验依据；后续属性布局缓存属于另一个独立假设，需重新证明增量收益。
