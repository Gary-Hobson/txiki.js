# 整数region覆盖不足：已证实结构与未决的退役策略

2026-11-10。**补充诊断后仍关闭当前纯数值region路线，不实现backoff或Number DAG变体。** 三个数字lexer确因正常Number溢出过早退役，但后续完整int32资格只能再增加约0.0018个百分点的主VM分派覆盖。原型未计分；[归档报告](quickjs-int-region-analysis.md)的覆盖裁决不变。这里区分源码可证明的屏障、实际省工作和退役后完整资格，不把任何一个百分比当作CPU耗时或速度提升。

## 1. 三处正常数字输入导致永久退役，不能忽略

在独立的一轮身份诊断中，4个首次失败的入口全部得到函数/PC和原字节段：[int-region-retired-sites.json](results/int-region-retired-sites.json)。诊断变换已归档为 [int-region-retire-profile-transform.py](experiments/int-region-retire-profile-transform.py)。它不改变优化策略，仅在原恢复canonical入口之前记录信息；未记录恢复后的执行频率。

| site | 实际位置（WTB dist/cli.js） | 原指令片段 | 首次失败 |
|---|---|---|---|
| Acorn readInt | 行66419，pc153 | get_loc2, get_arg0, mul, get_loc8, add, put_loc2, inc_loc | total=900719925，radix=10，digit=4；mul超int32 |
| Babel readInt | 行121996，pc474 | get_loc3, get_arg0, mul, get_loc8, add, put_loc3 | 同上 |
| Prettier内嵌parser readInt | 行175290，pc458 | 与上一项相同 | 同上 |
| Buffer.copy | 行202071，pc162 | get_arg3, get_arg2, sub, put_loc0, get_loc2, get_arg0, strict_eq | this/target为对象，INT-only比较guard失败 |

前三段就是数字扫描的 `total = total * radix + val`，Acorn还把循环i++放在同一区域。900719925×10是正常Number运算，应该转为double；原型正确回退，却也永久关闭了同一入口后续处理短整数token的机会。这是已证实的策略缺口，不再只是“也许有少数热site退役”的猜测。

身份日志本身不能回答退役后执行频率：只记录4次退役，绝不等于之后只有4次失败或只有4次丢失机会；最初0.079%描述当前永久退役策略的实际覆盖，不是630个site的潜在覆盖上限。身份诊断与此前覆盖诊断是不同run，分母不合并。

因此又运行一次独立的退役频率诊断：在当前frame的活 `b->int_regions` 内定位三个pc，不缓存裸PC/plan；canonical仍照常执行，只模拟全部输入/旧值guard和数值计算，在任何local提交之前返回资格，也不更改原命中/失败计数。包括退役后立即执行的那次canonical fallback，结果为：

| 退役入口 | 后续canonical执行 | 完整int32 DAG资格 | 对应可再省主VM分派 |
|---|---:|---:|---:|
| Acorn readInt | 10,564 | 10,536 | 63,216 |
| Babel readInt | 2,017 | 2,003 | 10,015 |
| Prettier内嵌parser | 295 | 281 | 1,405 |
| 合计 | 12,876 | 12,820 | 74,636 |

同run实际dispatch为4,154,651,584；按原型的saved_dispatches和retired重建参照为4,157,932,656。当前净省3,281,072（0.078911%），这些后续完整资格最多再省74,636，即 **0.001795个百分点**，合计 **0.080706%**。不把新run计数除旧run分母，也不把入口总频率称作可优化频率。

频率诊断先通过int-region/object-layout/private-bytecode三项JS正确性，归档 [变换](experiments/int-region-retired-frequency-transform.py) 输出与实际诊断源码逐字相同（SHA-256 `ad7946536367328db291e14e65db6673079bcee3f8bad7165f584768bf68a2e1`）；完整计数和方法见 [int-region-retired-frequency.json](results/int-region-retired-frequency.json)。此版本有额外每dispatch检查和dry-run，绝不使用吞吐评分。结论是：过早退役为真实策略缺陷，但本工作负载中它丢失的完整整数机会不足以挽救当前路线。

## 2. 源码显示数值通常嵌在对象、调用和分支之间

分析的是实际WTB安装依赖：Acorn 5.5.3、TypeScript 3.3.1、Esprima 4.0.0。依赖文件SHA、选定源码段、在31MB实际bundle中的对应行、已有opcode汇总都存入 [结构证据](results/int-region-structure-evidence.json)。这些段是WTB的tokenize/parse/transpile路径；没有逐函数self-time证据，因此不将某一段直接称为占比最高的热点。

**Acorn：位置是对象字段，字符来自方法调用。** `skipSpace`（acorn.js:4666；bundle:66135）的循环条件是this.pos与this.input.length，body先调用this.input.charCodeAt(this.pos)，随后switch字符、更新this.pos，并可能调用skipBlockComment/skipLineComment。`readInt`（acorn.js:4950；bundle:66419）同样先读字段、call charCodeAt，穿过多个条件分支，然后写this.pos，再执行可进入DAG的整数累计。原型只能取得末尾几条算术，不能跨前面的属性访问、call、branch或属性写。

**TypeScript：扫描状态在共享闭包中。** `createScanner`（typescript.js:6885）声明text、pos、end、tokenFlags等，由返回对象的方法和内部scan函数共享。`scan`（7423）先读写这些捕获变量，再进行pos>=end、text.charCodeAt(pos)和switch；`scanNumberFragment`（6944）还有isDigit调用、substring和字符串累加。对应的get/put_var_ref不是当前local-only IR支持的操作。要注意：外层createScanner因own var_ref_count被整函数排除，内部scan则可能只是在读外部closure时遇到指令屏障；不能把这两种原因混成一个“所有捕获函数都被排除”的统计。

**Esprima：整数索引伴随字符串构造。** `scanNumericLiteral`（esprima.js:5770；bundle:148713）反复组合this.source、this.index++、charCodeAt、Character.isDecimalDigit和num字符串拼接。即使源码看起来有很多“数字扫描”，其运行工作也不等于连续整数算术；对象属性更新、字符串分配和调用都切断本原型区域。

已有完整opcode诊断也支持这些结构确实频繁出现，但只用作独立的结构证据，不与新run相除：

| 旧诊断中的事件 | 次数 | 正确解读 |
|---|---:|---|
| get_field/get_field2/get_length/get_array_el/get_array_el2 | 698,647,047 | 大量数据producer不属于local/arg/constant |
| 各种get/put/set_var_ref | 153,055,912 | 共享闭包值访问有实际动态规模 |
| 各种if_true/if_false | 560,061,562 | 多个小块和poll边界，不能直接拼成一个纯区域 |
| 比较前一个被观测dispatch为属性/length/array producer | 82,831,593 / 396,523,299（20.89%） | 该类producer会切断当前IR；不是精确可融合比例 |
| 算术（含inc/dec）前一dispatch为上述producer | 14,488,532 / 59,155,516（24.49%） | 同上，且动态pair可跨调用 |

初版遗漏了可安全支持的push_i16，旧数据中它直接先于48.82M次比较，但直接先于算术仅0.21M次。它是一个真实覆盖缺口；然而大量前者仍是单比较，不满足6指令/2compute门槛，不能据此声称补一个常量opcode就能让架构获得大收益。

因此有证据的解释包含两层：可形成的纯数值片段本来被状态访问/调用/分支切得很短，**另外**三个选中的数字累计site又被正常overflow永久退役。当前证据不能给这两层分配精确的失去覆盖权重。

## 3. 下一步先判断退役损失，不能随意重新覆盖入口

同run退役频率没有达到值得继续的覆盖，因此本轮不实现策略变体。仍须保留一个通用约束：第一次改写的前5字节协议允许永久恢复，却不能在恢复后任意在新调用或回边重新写私有入口。旧canonical执行可能在这5字节内触发类型转换并重入，上一层frame的continuation可能仍落在被覆盖区域，重新覆盖会破坏它。不能用一个简单热计数重patch规避这个不变量。

原先考虑过以Number为初始guard、内部无装箱double的DAG，让正常int32溢出不再触发退役。它需要逐项证明原int64乘法到double的舍入等价、负零、NaN、位运算的精确ToInt32/Uint32、末端boxing以及32位x87的精度控制；还必须避免编译器把分开的操作收缩为FMA。当前没有实施，不假设扩大数值域会快于已有整数handler。实际频率已经不足，停止此分支。

## 4. 更有覆盖依据的未来producer区域：不是再做callee内部intrinsic

此前charCodeAt实验在js_call_c_function内提前返回，仍支付JS_CallInternal、class.call和C函数入口；六对-0.28%，已拒绝。已有native频率诊断记录20.65M次charCodeAt，其中13.19M满足flat string/int等纯子域。这证明存在一类反复产生整数的调用，不证明它们后面都连着可融合比较；全局动态pair也不能替代同函数PC链。

源代码中 `charCodeAt(position)` 接字符分类/比较、以及string length接边界比较的形态是真实存在的。若退役策略解释不足，下一项可检验的架构问题是：**在call_method位置已经拿到owned callee/receiver/args后，保留调用checkpoint，能否把纯内建producer及后继typed consumers放在一个区域中，省掉更外层调用框架？** 这与把单个builtin快路放进C dispatcher不同，也比仅融合已产生的两值谓词多一层工作，但尚无本站点频率和成本证明。

必须先满足以下边界，不能从“无分配builtin”直接跳到跨调用值流：

* callee依实际JS_CLASS_C_FUNCTION、函数指针、cproto/magic、参数及receiver类型识别；方法解析在原代码中执行。覆写prototype、getter、Proxy、bound wrapper等不得按名称猜测。
* 原JS_CallInternal的call poll仍发生。poll callback可以重入并改变局部/对象，因此不能把poll前验证的普通local/shape事实沿用到后继区；owned flat string、int实参及真正callee身份与后续可变状态要分开证明。fallback必须明确发生在poll之前还是之后，不能从poll之后整段重跑call导致重复poll或副作用。
* 原native stack-overflow检查仍需执行，异常realm、backtrace和frame位置需要证明。减少物理调用层数可能改变实际C栈余量，不能把“同一个检查函数”直接等同为相同的边界行为。
* 原call_method（quickjs.c:18584）在返回后按receiver、callee、args顺序释放，再push结果。callee可带用户设置的属性，释放其引用可能连带finalizer；不能把这套cleanup统一延迟到区域尾部。cleanup也是checkpoint，之后读取的可变值必须重新验证。
* 条件分支仍执行其自己的poll；call poll和branch poll不能合并。发生任何可重入checkpoint之后只允许从准确continuation回退，不能假装仍是当前int32原型的“零提交整段fallback”。

最小下一步应是**同站点**诊断：记录真实callee身份、纯子域命中、其后直到下一checkpoint的canonical指令形态和动态长度，区分call结果直接比较、存local后单比较、以及能接多个操作的区域；同时区分flat-string length与会触发对象语义的length。只有足够多的站点实际跨越昂贵producer且仍有有效typed consumers，才开始实现。这轮仅提出可检验设计，没有将旧borrow谓词、slot IC或单builtin负结果重新命名为成功路线。
