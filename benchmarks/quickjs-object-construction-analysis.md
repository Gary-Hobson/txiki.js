# QuickJS 普通对象初始化布局原型

2026-11-10。正式对照为 `ef801eaa` 的属性 hint、局部字段融合和比较条件分支融合，构建 `build/lto-benchmark/integrated-branch/tjs`；Release + ThinLTO，PGO/inspector 关闭。未改动 `deps/quickjs` 子模块或正式性能 patch。首版六对整机增量不稳定，已拒绝保留；唯一修正版（最终 shape 全局共享）验证中。

## 可执行方案及成本依据

`OP_object` 先创建零属性普通对象；每个静态 `OP_define_field` 调用 `JS_DefinePropertyValue`，重新查找 descriptor，再经 `add_property` 查全局下一步 shape、克隆共享 shape、按容量扩展 props。原始 `7b5f588e` 的小对象 arena 和合并 GC header 已是基线能力，本实验没有把它们当作新增优化。已有采样中的 `add_property` inclusive 3.74% 和 `JS_NewObjectFromShape` inclusive 1.88% 仅说明局部窗口内成本存在，不能相加或当成整个 WTB 的占比。

原型将整个安全初始化区域改为一次按最终 shape 创建对象，再按已知 slot 接管各 initializer 值。函数在编译完成/字节码读取完成、尚未可能执行时，用 canonical opcode 的栈 pop/push 和变长调用参数量证明对象仍在操作数栈上。每个 eligible 区域保存创建 PC、字段 PC/atom、字段数及最终 shape 强引用。仅对象创建入口二分查找 site；普通 local load 和普通属性 handler 没有计划查询税。

首版接受 2–64 个唯一静态数据字段的直线区域；禁止中途分支目标、跳转、iterator/catch 非局部栈行为、yield/await，以及触及目标的 dup/置换、computed、spread、method/home-object、`__proto__` 构造操作。它允许独立 initializer 中函数调用、getter、eval、嵌套对象和抛错；不改变这些操作的执行顺序。第一次由普通消费者取用目标后区域结束，不向后推测构造器 `this` 或动态字段。

首版源码快照位于 [object-layout-transform.py](experiments/object-layout-transform.py)，独占 `stage-object-layout`。私有 opcode 254/255 与另一个 read-IC 实验明确互斥。记录原 atom/PC，序列化及 atom 回收时重建 canonical bytes；没有扩展持久字节码格式，也不额外拷贝整个函数字节码。可选计划分配使用 runtime allocator，不设置 JS exception，失败即保持原字节码。eager 准备避免“外层帧已创建普通对象，initializer 递归触发 tier-up，返回后碰到新字段 opcode”的混合执行问题。

## 生命周期及语义边界

- `JS_NewObjectFromShape` 对普通对象不初始化 props。调用返回后、任何分配/回调/异常点之前，helper 将所有有效槽设为 `JS_UNDEFINED`，然后才把对象交给 VM 栈；异常清理和 GC 永远看到有效 JSValues。每对象额外的 N 次 undefined 写入是实际成本。
- 模板 shape 强引用由函数 bytecode 持有，新增 GC mark 边、free 路径和内存统计。其后的 add/delete/freeze 可能因模板持有而多做 shape clone，这是必须计入的代价。
- 每次创建比较模板 `shape->proto` 与当前执行 context 的 `class_proto[JS_CLASS_OBJECT]`。原生 `JS_SetClassProto` 换默认原型时重新建立模板；已有对象继续持有旧 shape。`JS_CallInternal` 的既有 realm context 选择保留。
- 提前分配完整 props 会改变 OOM 时点及峰值内存。目标是在任何失败位置正确回收已接管值、保留已执行 initializer 的顺序，不承诺与原版本相同的内存限额失败阈值。
- Inspector/dump 模式不准备计划；私有 dispatch 区间使用编译期断言。GC 或递归中活跃执行的 bytecode 地址从不变化。

## 已完成验证

1. Release ThinLTO 原型成功编译，无编译 warning/error。对照与候选均通过 `object-layout-correctness.js`：普通/嵌套字段、1000 次构造及后续改 shape、递归 initializer、副作用及 throw 顺序、枚举与数字 key 顺序、函数名称、重复/computed/spread/method/proto、async/generator 分支回退等。
2. `object-layout-native.c` 在对照与候选库均通过。initializer 主动调用 C checkpoint 进行 GC、对活跃函数序列化、`JS_SetClassProto` 换默认原型并递归创建；外层对象保持旧原型、内层和后续对象使用新原型。双方 20 次 callback 用例通过。
3. 对照与候选写出的字节码文件 `cmp` 逐字一致，双方均可读取并执行对方字节码。活跃 initializer 期间的序列化亦与首次序列化一致。
4. 独立诊断构建执行专项：计划 143、sites 301、静态字段 1085、计划总分配 18192 bytes；动态预规划对象 13943，普通 OP_object 1346；预规划字段 55284，普通 define_field 874。这包含 runtime 初始化，**不是 WTB 覆盖率或性能证据**。

第二遍扫描最初直接向最终字段表写临时结果，拒绝候选可能超出已分配容量；在第二次构建及任何测试之前已改成固定 64 字段 scratch，只有成功扫描才复制。这是实现阶段修正，未把有缺陷版本交付性能测试。

## 待完成及拒绝条件

Terra 已完成首版相对 `integrated-branch` 的完整 WTB 配对和完整套件，首版拒绝保留。WTB 诊断在配对之后单独运行，使用 `object-layout-diagnostic-build.py` 生成 GC 输出统计构建；计数不能当耗时。最终 shape 共享修正版需先通过原生内存和语义检查，再作六对增量；若仍不稳定即结束本候选。

原生精确失败注入与内存 fixture 已完成，结果见后续各节。首版每个 site 首次执行新建模板 shape，重复布局的强持成本已确认；仅允许一次有证据的修正。

## 首版整套结果：拒绝保留

双方完整预热后六对交错 WTB，对照是含 branch 融合的正式版。总分配对中位 **-0.14%**，胜 **2/6**；第二对候选明显受干扰，不能把该轮绝对退化单独归因于实现。尽管总分接近持平，子项分化稳定且显著，当前版本不能保留。

| 轮次 | 正式对照 | 首版候选 | 吞吐变化 |
|---|---:|---:|---:|
| 1 | 3.56 | 3.53 | -0.84% |
| 2 | 3.54 | 3.36 | -5.08% |
| 3 | 3.55 | 3.58 | +0.85% |
| 4 | 3.53 | 3.59 | +1.70% |
| 5 | 3.56 | 3.56 | +0.00% |
| 6 | 3.56 | 3.55 | -0.28% |

| 子项 | 配对中位变化 |
|---|---:|
| acorn | -1.58% |
| babel | +0.68% |
| babel-minify | +1.81% |
| babylon | +0.37% |
| buble | -3.98% |
| chai | +0.59% |
| espree | -5.98% |
| esprima | +9.13% |
| jshint | +0.88% |
| lebab | +0.38% |
| postcss | -0.24% |
| prepack | +1.21% |
| prettier | -0.19% |
| source-map | +0.78% |
| terser | +0.00% |
| typescript | +0.53% |
| uglify-js | -0.32% |
| Geometric mean | -0.14% |

完整原始数据：[object-layout.json](results/object-layout.json)。完整套件在显式 `TJS_HOME` 时为 503 OK、1 inspector skip、TCP abort 和 storage 两项失败：TCP 属已知对照失败；storage 把该环境变量当作子进程标志，正式对照在同环境同样失败，移除 `TJS_HOME` 后两方单项均通过，未发现候选新增失败。

## 原生内存证据

`object-layout-memory.c` 编译 1000 个具有相同六字段 literal 的函数，再分别创建 1000 个对象、delete/add/freeze 并释放。对象阶段对照 shapes=108/shapeBytes=34440；首版 shapes=1107/shapeBytes=170304，多 **999 份重复模板、135864 bytes**。计划及 bytecode 扩展另增 **104000 bytes**；此阶段总 used 差 **255736 bytes**。改形后对照 shapes=1107，首版=2107。释放函数和对象后两方 shape_count 均恢复 106，候选保留较大的全局 shape hash 表容量，不能将这部分误写成泄漏。

这些数据证明模板强持和未共享的实质内存成本，但不独立解释 WTB 的逐项性能差异。另一个源码可见成本是精确 `prop_size=N` 丢失原 2→3→4→6→9 容量策略在 N=5/7/8 等尺寸的余量，后续添字段可能更早触发 resize；下一步只允许基于诊断计数选择一次结构修正，不扫描阈值以迎合17项。

## 初始化失败恢复

`object-layout-oom.c` 使用公开 `JS_NewRuntime2` 自定义分配器，在字节码已编译后，对运行期前180个底层分配点逐一注入一次失败。180次均成功触发 JS exception；每次清除异常后 GC、重新执行三字段构造成功，已经执行的 initializer effects 保持严格顺序，再释放 runtime。该测试验证失败后回收/恢复，不比较两版完全相同的OOM触发时点，也没有替代 ASAN/GC-stress 验证。

## 按子项切片的机制诊断

单独诊断构建在每个 WTB 子项输出之后主动 GC，输出累计计数后取差。**额外 GC 改变后续 heap/shape 驻留，插桩亦改变执行成本；该次分数不参与性能结论。** 普通 `JS_CLASS_OBJECT` 的未使用 opaque 字段只在此诊断中标记原始字段数，正式候选未做此变更。`capacity_model_resizes` 仅把当前 resize 与原始字段数对应的 2→3→4→6→9 容量作反事实比较，未重建对象后续 delete/compact 历史，不能解释为实测 baseline 多/少的次数。

| 子项 | 预规划对象 | 2字段对象 | 后续resize | 后续clone | 初始容量反事实resize |
|---|---:|---:|---:|---:|---:|
| acorn | 237545 | 228342 | 6 | 3 | 0 |
| babel | 345970 | 6959 | 625 | 293 | 112 |
| babel-minify | 81598 | 5194 | 373 | 84 | 2 |
| babylon | 646 | 640 | 2 | 1 | 0 |
| buble | 53180 | 28072 | 2259 | 566 | 156 |
| chai | 4068 | 174 | 2 | 1 | 0 |
| espree | 7998 | 612 | 50 | 25 | 8 |
| esprima | 1801428 | 1240356 | 402974 | 202505 | 0 |
| jshint | 123470 | 81006 | 6359 | 1446 | 937 |
| lebab | 249459 | 119586 | 80 | 72 | 2 |
| postcss | 214008 | 207709 | 44466 | 256 | 0 |
| prepack | 105648 | 31660 | 420 | 47 | 263 |
| prettier | 79457 | 69546 | 14 | 12 | 2 |
| source-map | 152876 | 108336 | 12 | 9 | 0 |
| terser | 12058 | 214 | 7323 | 7116 | 6916 |
| typescript | 4962 | 2574 | 298 | 297 | 0 |
| uglify-js | 33724 | 1758 | 17276 | 15857 | 14438 |

运行前的加载阶段已经建立 7318 个 site 模板，计划累计分配 788336 bytes/13101 sites。17子项合计预规划 3508095 个对象、11043147 个字段。esprima 的预规划量明显最多，这与收益集中于该项相容，但不是因果耗时分解。espree 仅7998个预规划对象、50次后续resize/25次clone，不能用这些直接成本解释其约6%退化；acorn 的容量模型计数为0，buble为156，也不支持丢失余量是这些主要负项的首要原因。因而不据此盲目上调最低字段数；esprima 同时有1240356个两字段对象，也会被这样删掉。

另一个代码生成证据：首版 `js_object_layout_new` 在ThinLTO产物中没有独立符号，被内联进解释器；`JS_CallInternal` 的符号区间从47908增加到49664 bytes（+1756），栈框均为0x370但寄存器分配变化。这能证明执行器布局受到影响，不能仅凭大小断言退化来自instruction cache。

完整计数见 [object-layout-phases.json](results/object-layout-phases.json)，原生内存见 [object-layout-native-memory.json](results/object-layout-native-memory.json)。

## 唯一修正版：复用全局最终 shape

基于已经证实的重复模板成本，修正版在每个site首次使用时，以最终 hash、prototype identity、字段数、atom顺序及全部flags严格匹配全局 hashed shape。匹配时强持已有shape；没有匹配才建立首版相同的精确容量shape。**没有同时调整准入字段阈值、cold helper的内联策略或新shape容量策略。** 使用已有shape会自然继承其原始capacity，这属于共享结果；不能将后续收益全部归因于少分配模板。文件为 `object-layout-shared-transform.py` / `object-layout-shared-build.py`，隔离stage为 `stage-object-layout-shared`。目前静态转换已验证，等待独占编译窗口进行原生验证。
