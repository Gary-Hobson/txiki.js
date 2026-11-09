# 可移植字符串 intrinsic 调用实验

2026-11-10。继借用区域和 site IC 的实测拒绝后，本轮直接验证内建调用的执行层成本。**结论：当前 charCodeAt intrinsic 候选拒绝接入。** 对已保留对象布局版本的六对中位变化为 -0.28%，四对下降、两对持平，没有测到稳定正增量。实现仅转换隔离源码，不改 submodule 或正式 patch；不生成机器码、不要求特定 CPU、不依赖方法名称或未失效的 prototype 假设。

实际原型：[string-intrinsic-transform.py](experiments/string-intrinsic-transform.py)。只处理真实 `String.prototype.charCodeAt` 内建函数的 flat-string / int-index 子域。构建与测试交独立代理，计时基线是 `integrated-object-layout`（hint + fusion + comparison-branch + shared object layout）。主代理已完成对象方向的故障注入/ASAN审计并确认保留该版本，因此本实验衡量其上的新增收益，不与旧 branch 基线混算。

## 1. 真实调用成本与频率证据

源码行号基于未打 patch 的 `deps/quickjs/quickjs.c`。`OP_call_method:18584` 记录 caller PC 后调用 `JS_CallInternal:18054`，后者先执行 `js_poll_interrupts`；非 bytecode function 通过 class.call 进入 `js_call_c_function:17862`。此函数先检查 native stack limit，然后建立/发布 `JSStackFrame`、切换 realm、必要时补齐参数，再按 cproto 调用实际 C builtin，最后恢复 frame 链。

`charCodeAt:47192` 是 `JS_CFUNC_generic`、length=1、magic=0（注册在48596），先 `JS_ToStringCheckObject`，再 `JS_ToInt32Sat`，最后读取 UTF-16 code unit，释放临时 string 引用并返回 int/NaN。平坦 string + int 不需要 coercion、分配或 JS 回调；原输入本来由调用者持有，因此临时 string dup/free 可以省。

已有10秒 sample 中 charCodeAt self 为21/8522（约0.25%），整个 js_call_c_function self 为109/8522（约1.28%）；这只是早期窗口，不覆盖全部17项，更不证明所有 dispatcher 时间属于 charCodeAt。新方案仍进入 JS_CallInternal 和 js_call_c_function，不声称删除了整个 VM 调用链。

先在此前的 `integrated-branch` 上做独立频率诊断：[native-call-profile-transform.py](experiments/native-call-profile-transform.py)，原始 [完整运行日志](results/native-call-profile.txt)。这不是最终计时基线上的耗时归因。它按实际 function pointer + cproto/magic 识别，计数版的分数不用于评价性能：

| 调用类别 | 动态次数 |
|---|---:|
| js_call_c_function 路径合计 | 87,131,874 |
| charCodeAt | 20,645,674 |
| flat string + int 且其余 guards 合格 | 13,192,442 |
| 合格且下标在范围内 | 13,190,142 |
| 合格但下标越界 | 2,300 |
| charAt | 2,379,278 |
| codePointAt | 341,628 |
| slice | 4,840,803 |
| substring / substr | 102,614 / 306,289 |
| indexOf / lastIndexOf | 926,142 / 14,008 |
| includes / startsWith / endsWith | 642 / 86 / 0 |
| 未列出的该路径 C builtins | 57,574,710 |

这里的总数仅指 `JS_CLASS_C_FUNCTION` 的此 dispatch 路径，不包含 C_FUNCTION_DATA/C_CLOSURE 等其他 class 的 native 调用；也包含启动和非计分代码。charCodeAt 占该路径约23.7%，其中63.9%合格，合格调用占总数约15.1%。这些是选择一个窄原型的频率依据，不是耗时份额或 Amdahl 上限；未把不合格的7.45M调用未经测量归因于某一类型。

charAt 的 `js_new_string_char:4299` 会走新建8/16位字符串路径，可能分配/抛 OOM。它不属于此次“无 native frame、无可抛 helper”的证明范围，因此没有顺便加入。

## 2. 实际实现边界

快路放在 **js_call_c_function 原 stack-overflow 检查之后、frame/realm 发布之前**。把原本在 dispatch 前读取的 `JSCFunctionType func` 提前到这里，fallback 继续复用同一值；中间原有操作只有 frame 设置和参数补齐，没有执行 JS。普通 JS bytecode calls 没有新增 guard；其他 C builtins 首先只比较一次实际函数指针。

必须同时满足：

* `func.generic == js_string_charCodeAt`，cproto 为 generic、magic=0、内部 length=1。
* 没有 constructor flag，argc>=1。
* this 的实际 tag 为 JS_TAG_STRING（排除 rope、boxed string 和其他 primitive）。
* argv[0] 的实际 tag 为 JS_TAG_INT。

命中时用现有 `string_get` 读8/16位字符，返回 code unit 的 int；负数或越界返回原 JS_NAN 常量。所有输入仍是 borrowed，输出是 immediate；不写 JS 对象、参数或局部值，不分配、不释放、不调用 user code，也不改变 frame 链。原 VM handler 在返回后照常释放 callee/receiver/全部 arguments，所以多余参数及其求值副作用仍按原时机处理。

失败继续完整原 C 路径。覆写 String.prototype、用户函数恰好同名、method getter、Proxy apply、bound wrapper 都照常解析/执行；只有最终实际被调用的 builtin 身份相同才可能进入快路。保存原 builtin 后再覆写 prototype，保存的函数仍可安全走快路；没有缓存属性值或根据 name 属性猜身份。

## 3. 不变量

**中断与栈限制。** 原 JS_CallInternal 的每次 poll 原封不动；overflow check 也未移动或删除。快路在检查之后才执行。native fixture 已直接 JS_Call 50000次，核对原10000次检查节奏，并验证可中断和极小 stack limit 仍报错。

**realm 与异常。** 纯数字结果不依赖 callee realm，且命中区没有可抛 helper，因此可以不切 realm、不发布 C frame。Symbol、BigInt、float/undefined/default index、对象 coercion、boxed receiver、rope 和 constructor 情况全部走原路径，保留异常 realm 和原 frame。跨 realm 的 Symbol-index fixture 比较实际 TypeError prototype，避免只检查“某个错误发生”。

**所有权与 GC。** this 字符串由原 caller/C API 契约持有；快路没有分配、free 或回调，省略内部临时 dup/free 不会让 owner 消失。不是把未计数值放入 VM 物理栈，也不改 C API 的 consuming/borrowing 契约。

**Unicode 与数值。** 读取的是 UTF-16 code unit，保留 surrogate 单元而非 code point；范围外 NaN，空串同样 NaN。非 int 保守 fallback，包括 -0 的 double 表示、fraction、Infinity/NaN、String/Boolean/Symbol/BigInt index。

**物理调用栈与编译器。** 源码可证明省去的是 native frame 的发布/字段初始化、realm 切换、cproto switch 的实际 builtin 调用，以及纯子域转换与临时引用；编译器是否仍在函数 prologue 保留 stack space/保存寄存器尚未检查，不能把它描述成完全消除了所有机器栈成本。

**兼容性。** 无 bytecode/opcode/JSValue/API/正常对象布局变化。只有启用 `QJS_STRING_INTRINSIC_DIAGNOSTICS` 的诊断版增加 runtime hit counter，并在 JS_RunGC 输出 `STRING_INTRINSIC_STATS`；正式计分关闭。变换→debugger 与 debugger→变换两顺序纯源码 replay 已通过，不等同于实际 inspector 构建测试。

## 4. 验证与验收

专项 [string-intrinsic-correctness.js](experiments/string-intrinsic-correctness.js) 覆盖 Latin-1、UTF-16/代理对/孤立 surrogate、边界/空串、缺参/浮点/coercion、覆写/getter/Proxy/bind、extra args、求值顺序和非构造性。[native fixture](experiments/string-intrinsic-native.c) 检查 poll、stack limit、跨 realm 正常值与异常原型。独立代理在 baseline、候选和诊断版本上均通过这些测试；对象布局和 private-bytecode 既有专项也通过。native fixture 的50000次直接调用实际触发5次 interrupt callback，正常和候选一致；中止、stack overflow 和异常 realm 检查均通过。

命中不是推测：诊断版字符串专项记录2114次命中，native fixture记录50000次，另一个强制定向检查记录475次。可复查 [验证与计时日志](/Volumes/zhitai/workspace/lite-engine/txiki.js/build/lto-benchmark/quickjs-hotpaths/string-intrinsic-screening.log)。没有运行实际 inspector ON 构建，此处只声明前述两种 patch 顺序源码兼容；候选已被性能门槛拒绝，不继续追加该构建。

## 5. 六对结果与拒绝理由

计分版本关闭诊断，使用相同 Release + ThinLTO 配置，在 baseline 和候选各预热后交替先后顺序运行六对。完整17子项及逐次原分保存在 [object-string-intrinsic.json](results/object-string-intrinsic.json)。以每对几何均分比值计算：

| 轮次 | integrated-object-layout | + charCodeAt intrinsic | 变化 |
|---|---:|---:|---:|
| 1 | 3.61 | 3.57 | -1.11% |
| 2 | 3.60 | 3.59 | -0.28% |
| 3 | 3.60 | 3.60 | 0.00% |
| 4 | 3.60 | 3.60 | 0.00% |
| 5 | 3.61 | 3.60 | -0.28% |
| 6 | 3.61 | 3.60 | -0.28% |

配对变化中位 **-0.28%**、均值 **-0.32%**，**0/6胜、2/6平**。分项中位改善较大的有 babel-minify +0.81%、buble +0.56%、postcss +0.48%；较差的是 chai -1.02%、prepack -1.00%、uglify-js -0.79%、source-map -0.46%。原分仅保留两位小数，不能把这些小差异解释成精确的独立因果效应；但本轮没有支持接入所需的稳定整体正收益。

测试证明了可移植的无 native-frame 纯子域语义可行，未证明它在整个 workload 上合算。此实现仍经过原 JS_CallInternal poll、class dispatch、js_call_c_function 入口和原 stack check；它增加了该入口其他 C calls 的身份判断，且编译器可能仍保留部分物理 frame 成本。现有数据不能分离 guard 税、代码布局或已有编译器优化的影响，不将任何一项写成已证实的退化原因。

本候选保持为可复现的隔离实验，**不写入正式 patch，不默认扩大 rope/charAt/coercion 支持，也不按调用频次承诺倍率**。只有新的成本证据支持更大且可安全消除的调用开销时，才有理由重新选择调用架构；当前这一窄子域已完成测试并收敛为拒绝。
