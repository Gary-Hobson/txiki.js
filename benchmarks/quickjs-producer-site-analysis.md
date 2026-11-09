# 纯内建producer的同调用点后继诊断

2026-11-10。本轮只统计，不修改解释器执行策略。基线为已保留regex优化的 `integrated-regexp-anchor`（正式engine fa509604），沿用该版本的QuickJS/RegExp archive和link配置；不混用此前object-layout实验的旧libregexp。

已归档 [producer-site-profile-transform.py](experiments/producer-site-profile-transform.py)。目的不是把此前charCodeAt intrinsic的六对负结果重新命名，而是回答更外层的问题：真实纯charCodeAt调用的结果，是否在同一JS调用点后面接着足够多的可特化计算，值得进一步研究跳过JS_CallInternal/class.call层的producer区域。**诊断结论：不是多运算DAG，而是高频的内建整数producer→local store→单比较/分支。** 当前只完成统计和候选设计，尚未实现跨call优化。

## 1. 如何证明调用者与调用点，而不是拼接跨调用pair

插桩位于 `JS_CallInternal` 内的 `OP_call_method` handler：原handler先读取2字节argc，设置 `sf->cur_pc=pc`，执行原JS_CallInternal调用并检查异常。本诊断只在成功返回后、原receiver/callee/args清理之前运行，且不处理tail_call_method。传入的是该handler的 `sf/b/pc/call_argv`，不是在native helper中猜测当前frame来自哪里。

诊断逐次核对：

* 实际callee对象class为C_FUNCTION，函数指针为js_string_charCodeAt，cproto=generic、magic=0、内部length=1。
* receiver为实际flat string tag，argc>=1且第一个index为INT；分别记录越界和额外参数。只有in-bounds结果进入整数producer后继分析。
* 返回后的current_stack_frame恰是原sf，sf.cur_func为bytecode function且其bytecode指针等于本b，sf.cur_pc等于pc；pc[-3]为call_method、其argc等于原argc，实际返回结果tag为INT。

Proxy、bound function和Function.prototype.call外层的callee身份不等于charCodeAt，因此不会因它们内部又调用真实builtin而误归为直接site。Native C API嵌套调用也不会生成新的call_method插桩事件。另一方面，若原属性getter最终返回真实builtin，已有方法解析已经执行，本诊断可以计入随后的直接call；这不证明getter可以从未来区域中删除。

插桩不移动原调用、poll、stack check或清理，不执行后继指令，不读写局部值，也不保存JSValue/atom/bytecode/PC裸指针。运行时只持有整型计数和最多256种opcode形态。每次都由当前活b解码其当前pc；新形态的代表函数名/行/PC立即打印，后续聚合不依赖这些地址，避免GC复用或ABA。

## 2. 后继形态与统计边界

从此调用返回后将产生的一个结果值开始，用最多16槽的虚拟taint栈追踪它流经get/put/set_loc、dup、常量和算术/比较的形态。局部转存使用版本化taint映射，后续覆盖该local会清除旧taint。只看同一函数PC起点后的最多24条canonical opcode，遇到branch、call、property、closure access、return或其他不支持操作即停止；正式object私有254/255映射成object/define_field屏障，不解读私有operand。

分类为互斥桶：多次依赖producer的compute优先记multi_compute；否则比较按是否先存local区分direct_compare/local_compare；另有arithmetic、store_only、truthiness、other。同时统计停止原因、所有in-bounds producer数、正常函数+单参数+无own captured locals的保守子集，并记录最常见opcode形态。

**这些计数是成功producer调用加权的后继语法形态，不是已观测完整执行的后继长度，更不是已满足所有类型/所有权guard的region覆盖。** 后续算术可能因其他operand类型而coercion/抛异常，call cleanup可能触发finalizer，局部旧值可能有refcount，外部closure访问另有alias证明；本诊断没有抹去这些条件。24条上限和预存VM栈不足也分别记为停止原因。不同调用点可能被聚合为同一形态，因此形态数量不是unique site数量。

## 3. 验证与结果状态

专项 [producer-site-profile-correctness.js](experiments/producer-site-profile-correctness.js) 分别执行20次直接比较、存local再比较、多计算和嵌套后继调用，最后执行Function.call/Proxy/boxed receiver；GC快照的in-bounds纯直接调用增量为20/20/20/20/0，分类检查通过。运行环境在启动时也会自动GC，验证使用这段连续增量而非固定“第几个快照”，避免把启动期统计当fixture错误。baseline/诊断版同时通过既有string-intrinsic、object-layout和private-bytecode专项。

完整一轮统计见 [producer-site-profile.json](results/producer-site-profile.json)。归档变换输出与实际测试 `stage-producer-site/quickjs.c` 逐字相同，SHA-256 `2dfb166e1cf5a9fd93fee35c1aa517212066ef6efe01063b45d25a43b7adf4f5`；baseline quickjs.c为 `4025d0bc28c45b8a68aaccd603b0309cfe53fef70dc37f3e3957f133fdf29d58`，libregexp.c为 `24ea589d535ab3a58627b64ba795647aa62eb94f60ee9bcd2eaf9d496c8d8dde`。没有评分这个插桩版本。

| 指标 | 次数 |
|---|---:|
| 成功call_method | 126,319,295 |
| 真实直接charCodeAt身份 | 20,645,353 |
| flat string + INT纯域 | 13,192,121 |
| in-bounds / out-of-bounds | 13,189,821 / 2,300 |
| 单参数 / 额外参数 | 13,192,121 / 0 |
| caller/frame不变量失败 | 0 |
| direct_compare | 86,549 |
| local_compare | 10,871,699 |
| multi_compute / arithmetic | 0 / 0 |
| store_only / other | 1,492,002 / 739,571 |

in-bounds中的82.42%属于local_compare。全部local_compare和direct_compare也满足“普通函数、单参数、无own captured locals”的元数据筛选，但没有测量local旧值类型，因此这不是所有权完整资格。12,019,239个shape停在branch，277,772停在call，289,137停在property，105,406停在closure；最大长度和invalid均为0，pattern表无overflow。

三个最常见后继占10,321,886次，即全部in-bounds的78.26%：

| 同调用点后继 | 次数 | branch之后需保留的值 |
|---|---:|---|
| set_loc0, push_i32, gt, if_false | 4,814,812 | 无；local保留code unit |
| set_loc0, push_i32, lte, dup, if_true8 | 3,458,114 | boolean；local保留code unit |
| set_loc1, dup, push_i8, strict_eq, if_true8 | 2,048,960 | 原code unit；local也保留它 |

第二种dup位于比较之后，第三种位于比较之前，不能共用错误的stack-effect描述。第一形态的首次代表来自Buffer的utf8ToBytes，第二来自fullCharCodeAtPos，第三来自Acorn skipSpace；一个形态可聚合多个函数，因此不能把整桶次数全部归给代表函数，也不能把所有producer叫作lexer热点。

## 4. 有根据但尚未实施的候选边界

值得考虑的最小候选是这三类**call producer + local store + compare/branch**。它与此前单callee intrinsic的区别在于入口移到VM的call_method位置：命中才有机会省去调用JS_CallInternal的物理C函数入口、非bytecode class.call间接调用、js_call_c_function的frame发布/realm/cproto dispatch，再省去charCodeAt的转换/临时引用以及后继的VM栈搬运。这里的主要新增假设是更外层调用成本，不能只用省dispatch来解释。

以当前保留的comparison-branch融合为参照，三个shape的后继最多分别再合并3、5、4次主VM dispatch；按本次形态计数对应39,930,846次潜在消除。但它尚未扣除local旧值、输入域、metadata、guard和退役策略，且本诊断没有同run总dispatch分母，故不计算百分比，也不把它当作周期或速度提升。

执行流程必须保留两个checkpoint：

1. 在已有owned callee/receiver/arg上检查实际builtin身份和纯域。未进行任何poll的guard失败直接进入原JS_CallInternal调用。命中路径仍设置原sf.cur_pc，执行原call poll和native stack-limit检查。poll可以重入；只有已证明不可变且由VM栈持有的callee内部身份、flat string和INT实参可跨越它，不能把普通local/shape事实顺带带过去。异常使用原caller realm和原exception清理路径。
2. 得到immediate code unit后，严格按原call_method顺序释放receiver、callee、args。它们的清理可能连带finalizer，这个checkpoint不可延迟到suffix末端。之后才重新检查写入local的旧值为INT/UNDEFINED，并读取任何其他可变状态。suffix guard失败时，按原call返回状态push结果、从call末端继续，绝不重新调用builtin或再执行一次call poll。

suffix命中后只做immediate local store、整数比较、三种明确的结果保留方式，最后按原条件更新PC并执行原branch poll。call poll和branch poll是两次，不能合并。物理调用层数减少会改变真实C栈余量，不能以“调用了同一个检查函数”声称所有stack-limit阈值完全相同；stack overflow、poll重入、原异常realm/backtrace仍须独立验证。

callee-body优化正在由独立路线验证flat/cached-rope + INT借用快路，保持全部原call/frame/poll。若该版本被保留，这里后续候选必须在它之上测新增收益；不能再次认领已由body省掉的转换和临时RC。当前producer统计只涵盖flat string，未测cached-rope后继形态，不能直接把其他诊断的rope计数加进本次覆盖。

## 5. 3字节入口、非目标退役与剩余成本

候选若实施，可用等长的私有call opcode加16位site index替换原call_method的3字节，原argc保存在plan。后继suffix完全不改写，原branch target仍指向原字节位置；序列化和销毁恢复这3字节。正常opcode空间、inspector的252/253以及已有object254/255仍须编译guard，不能无条件占用编号。冷函数sizeof、计划字节、创建扫描成本仍必须测量。

这比此前跨5字节覆盖多个opcode的intregion更容易证明continuation稳定：原handler在任何JS_CallInternal、poll或其他可回调操作之前已经读完argc并令pc指向call末端。回调/递归期间保存的sf.cur_pc因此在3字节之外；把入口恢复为原opcode+argc不会改写active frame的continuation。单线程解释器且inspector/dump禁用时，原opcode读取与argc读取之间没有可重入点。即使另一递归层退役了site，当前handler已经取到的recipe仍需保留到所属bytecode释放，不能立刻释放plan；当前调用可据原owned操作数正常完成。

为避免同形态但非目标callee长期额外付税，候选筛选策略可以在第一次**实际callee身份不符**时永久恢复该site的3字节，未来完全回到canonical dispatch。此策略会丢失“第一次是其他callee、后来才变charCodeAt”的机会，必须计数，不伪称覆盖全部调用。实际callee为charCodeAt但输入域不符，不能不加区分地永久退役：flat/rope或INT/其他index可能在同一site交替；可继续用原argc执行普通call，不重写suffix、不重跑已经完成的call。非目标callee和目标callee域外失败是不同成本问题。

当前还缺一个决定框架税的分母：**所有callee中静态匹配这三类suffix的调用总量及首次身份失败率**。现有诊断在确认charCodeAt之后才做形态解码，不能据10.32M目标调用推断非目标guard开销很小；local旧值为可直接替换immediate的比例也未测。因此下一步只能是这个候选筛选/旧值门槛的诊断，或在明确这些限制后实施隔离原型再实测。此报告不把它视为已经获益的优化；当前没有新生产候选。
