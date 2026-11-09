# 按访问点的普通属性读取 IC 实验

2026-11-10。比较基线是 `build/lto-benchmark/integrated-final/tjs`，已含 slot hint 与 load-field fusion。原型只在隔离构建转换源码，不改 submodule 或正式 performance patch。独立变换已归档为 [site-read-ic-transform.py](experiments/site-read-ic-transform.py)，快照与纯源码 replay 说明见 [experiments/README.md](experiments/README.md)；v1 快照比首测仅增加后来补充的编译期断言。本文件记录一个实际可测原型，不承诺倍率；先前 borrow 区域组合 -1% 的结果继续保留在 JIT 专项报告，不将其功能带入本实验。

## 1. 为什么选这条边界

slot hint 每次仍检查 slot 范围、descriptor atom 与 flags；普通 hash 查询还要计算 bucket 并跟踪 descriptor 链。此原型按 bytecode 访问点记录两个 `{layout_id, slot}`，命中直接从**当前**对象的 `prop[slot]` 读值并 dup，省去 descriptor/hash 查找。不是缓存字段值，普通赋值后仍读最新值。

全量布局版本方案需要审计每个 mutation transaction，并承担新增写入和重入处理。本次源码给出一个更窄但可证明的资格：**只缓存 `JS_CLASS_OBJECT && !is_exotic && shape->is_hashed` 的自有普通数据属性**。descriptor、删除及 prototype 修改前，`js_shape_prepare_update` 会把对象切换到 unhashed shape。它成为已有的失效边界；不重新制造一个覆盖全对象系统的版本协议。

不缓存 prototype 命中、missing、accessor、autoinit、varref、数组、typed array 或 Proxy。shape ID 不是完整 ECMAScript 对象状态的版本，只保护已缓存 own-data 属性的含义。这一限制对正确性和后续扩展都很重要。

## 2. 实际数据结构与开销

* `JSRuntime` 增加 64 位单调 `read_ic_next_layout_id`，calloc 初值 0。
* `JSShape` 增加 64 位 `read_ic_layout_id`；只有首次填入 IC 才分配非零 ID。不是地址，不持 shape 引用，不增加 GC 边或为保留缓存额外触发 clone。
* `JSFunctionBytecode` 增加 plan 指针与 8 位饱和 warmup（结构对齐增量需由实际构建测量）。普通调用或 coroutine 恢复第 32 次尝试准备一次。超过 65535 字节的函数跳过；失败或没有 eligible site 不再反复分配。
* plan 一块 allocation：header + `32 * site_count` 字节的双态 entries + `bytecode_len` 字节的 canonical copy。没有按 bytecode_len 建 uint16 map。entry 保存两个 ID/slot、原 atom、累计 miss 数、替换 way 与原 opcode。atom 只是原字节码已有引用的副本，不另增引用。

plan 的 allocation count 和完整 bytes 已计入 `compute_bytecode_size`；shape 增量由现有 `sizeof(JSShape)` 相关统计涵盖。诊断累计 bytes 是总创建量，不能当峰值 live bytes。即使函数永远冷，新增 function/shape 字段仍有全局税；不能只看 cache hit rate。

## 3. 框架税与已有 fusion 的协作

现有 fusion helper 和全部 11 类 local/arg handlers **完整保留**，不加 table 查找、不多传 b 参数。canonical 扫描发现 `get_loc/get_arg` 的受支持形式紧邻 `get_field`，就不 quicken 该 field；已有融合继续省 receiver dup/free 和 dispatch。`get_loc0_loc1` 不在既有融合支持名单，不需要排除。

其余 `get_field` 和 `get_field2` 才使用私有 opcode 254/255，4 字节 operand 改为 site index。两条新 handler 从原 handler 克隆；普通 handler 没有额外 opcode 判断。快路的代价是 plan/site 寻址、对象/class/exotic/hashed/ID guards、至多两个 ID 比较；慢路继续原 hint/prototype/helper 语义。

命中对 `get_field` 先 dup 结果再 free receiver；对 `get_field2` 保留 receiver 做 this。miss 后只有实际找到原始 receiver 的普通 data descriptor 才 fill；prototype 的 property 不填。填充没有分配、释放或回调。双态满后轮换替换；累计 32 次 miss 时恢复该 site 的原 opcode 与 atom，后续彻底回普通 hint handler，没有长期 MEGA 额外分支。不是“32 次连续 miss”，一个足够久的低频多态 site 也最终会退出，这可能牺牲潜在收益但限制持续回归。

被跳过的融合字段意味着覆盖受限。这个取舍先测剩余字段的增量，不让新架构抹掉已保留的小优化；若收益不足，不能把全部 get_field 频次当本原型上限。

还需比较**两种命中路径**，而非只比较 IC hit 与完整 hash lookup。现有 hint 命中只需 slot 范围、当前 atom、TMASK 检查；新 IC 删除 descriptor 访问，却增加 plan/site 寻址和 bounds、class/exotic/hashed/identity guards、way 比较与 slot 读取。哪条依赖链更便宜取决于 C 优化和缓存行为；源码没有证明新路一定更短。当前未统计这些 IC hits 中有多少本来会 hint-miss，所以 99% IC 命中率也不能解释成 99% 次完整哈希链被省掉。

## 4. 布局身份与全部相关写点审计

以下行号基于未打性能 patch 的 `deps/quickjs/quickjs.c`。审计使用全文件 `is_hashed`、descriptor `flags/atom`、`proto`、`prop_count` 写点及 `add_property` 调用点，不只读一个 helper。

| 写点/事件 | 源码位置 | 原型处理与不变量 |
|---|---|---|
| new shape | 5686–5711 `js_new_shape_nohash` | 每次新 allocation 的 ID 显式置 0，防 arena 地址复用。未暴露的初始化不可能已有 IC。 |
| 第一次置 hashed=true | 5732 `js_new_shape2` | 来自上述 ID=0 的新空 shape。 |
| clone | 5781–5805 `js_clone_shape` | memcpy 后 ID 显式清 0，不能继承被分叉 shape 的 ID。否则不同新字段可能位于同一 slot 却误命中。 |
| 第二个 hashed=true 写点 | 10194 `add_property` | 仅把刚 clone 的 shape 放入 hash 表；该 clone 已清 ID。全文件只有这两处 true 赋值。 |
| existing shared shape 切换 | 10176–10190 | 使用目标已有 shape 的身份；目标布局本来相同，不强持 cache 引用。 |
| hashed 原地 append | 5996–6040 `add_shape_property` | 只追加末尾 descriptor，不改变已有 atom/flags/slot。保留 ID 对既有 own-data 读成立；不存在 negative/prototype cache。 |
| append 的 absence 前提 | 9313、9397、9421、10770、11129、11518、11768 等 `add_property` callers | 普通 setter/define/private/global 路径先检查不存在；bulk shape 初始化尚未暴露；ref-ref 构造用新空对象；module/array 特殊 class 不可缓存。literal 重复键走 define/update，不能借此向已有可缓存对象追加同名 descriptor。 |
| shape/props resize | 5847–5917 `resize_properties` | header/descriptor 复制或 realloc 保留 ID；已有 slot/flags 不变。每次 hit 使用当前 p->prop，不缓存搬迁前地址。失败路径也不改变已有 slot 含义。 |
| delete/compact | 10280、5921 | delete 在释放旧值前 prepare_update；compact assert 非 hashed，故 hit/fill 拒绝。slot 重排后的旧 ID 不会命中。 |
| descriptor flags | 11214 `js_update_property_flags` | 写入前 prepare_update；freeze/seal 等走此路径。 |
| data/getset/varref 转换 | 11295、11337、11373 | 释放旧 payload 前 prepare_update；直接 flags 写点均在其后。 |
| autoinit | 9097–9111 | 调初始化函数前 prepare_update，回调/失败/flags 清除期间都不可缓存。 |
| prototype 改写 | 8749–8756 | free 旧 proto 前 prepare_update。own-read 本身不依赖 proto，但仍保守退出。 |
| fast array → properties | 10220 | 先 prepare_update；且 class gate 排除。 |
| plain value 替换 | 2932 等 | 不改变布局，不失效；IC 读当前值。既有释放/重入顺序不修改。 |
| shape 回收 | 5820 等 | cache 只存数字，无 dangling pointer/GC owner；后续新分配明确 ID=0。 |

`js_shape_prepare_update:11186` 对 shared hashed shape 先 clone（新 ID=0、is_hashed=false），再切 p->shape；对 unique hashed shape 先 unlink 并设 false；非 hashed 继续保持 false。全文件不存在把一个已经 descriptor-mutated 的旧 unhashed shape直接重新设 true 的路径。因此 native finalizer/autoinit 等重入发生时，缓存命中与填充都先拒绝它，不会在中间布局重新填入“最新版本”。不需要单独 mutation bit，也不依赖“释放绝不会重入”的假设。

hashed append 中暂时 unlink 后 relink，但没有执行 JS/host callback；resize 使用 runtime allocator 路径，atom dup 只是引用操作。共享 shape 追加先 clone；unique shape 没有另一个仍使用旧分叉布局的对象。由此允许保留 ID。将来若新增同名 append、hashed descriptor 改写或 unhashed 再 hashing，必须扩失效协议，不能直接沿用这份证明。

ID 到 `UINT64_MAX` 后不再分配新身份，现有身份仍可命中；未获 ID 的 shape 走普通路径，绝不回绕重用。不同 runtime 的 cache 与 shape 不交叉；本原型沿用 QuickJS runtime 不可并发执行的前提。

## 5. Lazy 原地 quickening 与 PC/字节码兼容

当前 `quickjs.h:1317` 的 `JS_READ_OBJ_ROM_DATA` 定义为 0，注释为 obsolete；reader 不保留 ROM 指针。全文件 byte_code_buf 仅两处赋值：编译创建 37134 指向 b allocation 的 self buffer，读入 39662 `bc_get_buf` 复制到同样的 self buffer 后 39665 赋值。变换精确断言这两处代码与总赋值数；支持源码范围内的 buffer 是可写的，不是只因指针非 const 就假定可写。

prepare 先完整只读扫描，遇非法 opcode/size 整体放弃；唯一可选 `plan = js_mallocz_rt(rt, bytes)` 失败直接返回，不设 JS exception。分配后先复制 canonical、发布完整 plan，再在无 callback/alloc 区间逐 site 写 operand 和 opcode。byte_code_buf 的地址始终不变，指令尺寸和所有相对/绝对 offset 不变。普通 active frame、递归外层、暂停 generator/async 都无需 PC 转换。

tier-up 可以在递归深处发生。原 `get_field/get_field2` handler 在任何可能调用 JS 的路径前都已经读 atom 并 `pc += 4`，因此恢复时继续使用局部 atom；其他暂停 opcode 不读这里被改的 operand。现有 local-field fusion helper 没有重入，且其字段 site 未被改写。并发执行同一 runtime 不在原引擎支持范围，不能把本方案描述成多线程自修改代码协议。

序列化始终复制 plan->canonical，私有 site index 不会写出；释放函数时把 canonical 拷回原 self buffer，再释放 plan，原 free_bytecode_atoms 遍历正确 atom。plan 只含数字和原 atom 的非 owning 副本，无新增 mark 边。编译创建和反序列化都使用零初始化；早期失败的空 plan 直接沿原释放路径。BC_VERSION、公有 opcode 表、C API 和 JSValue 表示不变。

私有编号有 compile-time overlap 断言；非 inspector 的默认 dispatch 范围另断言 OP_COUNT<=253。正常输入没有私有 opcode；没有 plan/越界 site index 的私有 handler 转原 invalid opcode 路径。原 reader 对不可信字节码不是完整验证器，本实验不把这些 guard 描述为恶意 bytecode 沙箱。

ENABLE_DUMPS、inspector header 宏 JS_DEBUG_TRACE_DEBUGGER_STMT、显式 QJS_DISABLE_READ_IC 均禁用 warmup/prepare。已静态验证 IC→debugger 与 debugger→IC 两顺序；真实 ON/OFF 编译仍需独立确认，禁用运行不自动证明编号和编译兼容。

## 6. 诊断、测试与结果记录

编译宏 QJS_READ_IC_DIAGNOSTICS 默认关闭，累计 plans/sites/bytes/layouts/calls/hits/misses/unhashed/retired；JS_FreeRuntime 输出 QJS_READ_IC_STATS。QJS_READ_IC_VALIDATE 可在每次 hit 检查 bounds、atom 和 TMASK，正式计分关闭。注意直接去掉 NDEBUG 会启用 QuickJS ENABLE_DUMPS 并禁用 IC；验证构建应仅独立启用 assert。WTB 的 tjs.exit 不一定执行 FreeRuntime，完整诊断需自然退出或单独在 JS_RunGC flush 后显式调用 GC，不改计分二进制。

专项：[site-read-ic-correctness.js](/Volumes/zhitai/workspace/lite-engine/txiki.js/build/lto-benchmark/quickjs-hotpaths/site-read-ic-correctness.js)。强制用 call receiver、array[i].x、第二级 chain、method call 避开既有 fusion；检查 append/realloc、clone 分叉、flags/type/proto/delete/compact、Proxy、GC 地址复用、双态/三态退化、活跃递归、先暂停 coroutine 后 tier-up 和 catch/finally。复用 native canonical 字节一致与跨版本双向读入测试；分配失败注入需确认第32次执行确实走到 plan allocation。

这里列出的专项/native fixture 链接仍指向本机隔离实验目录；可移植源码快照和评分 JSON 已使用仓库内相对路径，不把本机 build 目录当作已归档产物。

最初 [JS active serialization 用例](/Volumes/zhitai/workspace/lite-engine/txiki.js/build/lto-benchmark/quickjs-hotpaths/site-read-ic-active-serialization.js) 在 integrated-final 基线就因 `unsupported object class` 失败：tjs.engine.compile 固定使用 MODULE，模块执行后不能再按该路径写出。这不能归因于 IC，也不能记作测试通过。独立代理改用 [native active fixture](/Volumes/zhitai/workspace/lite-engine/txiki.js/build/lto-benchmark/quickjs-hotpaths/site-ic-native-active.c)，通过 GLOBAL|COMPILE_ONLY 保留可写出的句柄，在 active frame 内的 C callback 做 JS_WriteObject 比较。

替代 native active fixture 已在基线、IC 与诊断版通过；递归深处发生 tier-up，同时仍保留外层 active frames，canonical 输出逐字节一致。该 fixture 诊断 plans=1/sites=1/calls=1/misses=1，验证的是活跃 PC/序列化边界；真实命中证明来自下述扩充专项，不能把这一次 miss 写成 cache-hit 测试。

测试代理最初的独立定向诊断已经产生真实执行证据：plans=9、sites=38、layouts=32、calls=1038、hits=674、misses=364、unhashed=5、retired=11。扩充专项在基线、正式候选与诊断版均通过，诊断为 plans=22、sites=57、累计 bytes=3463、layouts=24、calls=1838、hits=1413、misses=425、unhashed=9、retired=10。命中后 descriptor 验证断言通过。这证明主要路径被触达，不代表 WTB 命中率。

独立完整 WTB 计数版（在退出前显式 GC 输出）得到：plans=5192、sites=35779、累计 bytes=2,103,425、layouts=6404、calls=132,214,010、hits=131,842,943、misses=371,067、unhashed=17,052、retired=9900。活动 IC 指令的条件命中率为 **99.72%**，累计建出的静态 site 中 **27.67%** 已退回原 handler。该命中率不包括退休后的普通执行、未 quicken 的冷函数或保留 fusion 的字段，也不是所有属性访问的覆盖率。其他 instrumented 运行的 opcode 总数不能混来计算精确覆盖；频次更不等于耗时。

## 7. 第一版增量结果与成本分解

原始 [六对结果](results/final-site-ic.json)：

| 对次 | integrated-final | + site IC | 配对变化 |
|---:|---:|---:|---:|
| 1 | 3.51 | 3.47 | -1.14% |
| 2 | 3.50 | 3.44 | -1.71% |
| 3 | 3.51 | 3.46 | -1.42% |
| 4 | 3.33 | 3.27 | -1.80% |
| 5 | 3.46 | 3.46 | 0.00% |
| 6 | 3.11 | 3.36 | +8.04% |

中位 **-1.28%**、均值 **+0.33%**、**1/6 胜**。第 4 对与外部构建相交，第 6 对基线异常低，不能用它把前几对的负结果包装成均值收益。本版不接入正式 patch。子项配对中位最差 terser -3.63%、esprima -2.26%；prepack +1.65%、babel-minify +1.27%，不能推断所有 AST 工作都会受益。

主代理检查完整 Mach-O `__text`：integrated-final 4,086,184 字节，候选 4,084,680 字节；整个 binary 的 text 没有增加，因此负结果不能直接归因于“全二进制膨胀”。局部 handler 布局/inline 决策仍可能不同，但尚无可靠时间或硬件计数证据证明其贡献。

下一步只有最小成本分解：同一 struct/layout 编译 `QJS_DISABLE_READ_IC`，作为**保留新增字段和初始化的禁用 IC 框架对照**。该宏也去掉 warmup/prepare，LTO 可能删代码和重排，因此不能把两版差额全归因于 sizeof(JSShape)，也不是严格只改变对象布局的因果隔离。它只能帮助判断总体框架负担。若证据支持，可独立验证一次有针对性的修正：用原先 is_hashed 字节及对齐空间容纳 `is_hashed:1 + saturating layout_id:31`，shape 不增加体积，ID 不回绕；相应 entry 可从32降到24字节。packing 若初筛正向，仍需独立六对，不能据框架差额预报收益。

验收需要同时看总分/17子项、完整 WTB hits 与退休比例、实际驻留/RSS 和基础 layout 字段税。若大量 site 被退休或被 fusion 排除，剩余覆盖可能不足；命中率高也不等于查询删除的时间足以覆盖 guards。不能拿 hit 次数换算耗时，也不能因为已经实现 IC 就继续扩大支持范围。

## 8. 禁用框架筛选与唯一 packed 修正版

[禁用 IC 框架对照结果](results/final-site-ic-disabled.json)：双预热后三对 baseline 均 3.50，禁用 IC 候选分别 3.46、3.48、3.12；配对中位 -1.14%、0/3 胜。第三对存在显著干扰，不能用均值 -4.19% 估计框架税；前两对 -1.14%/-0.57% 仅支持框架存在负担的筛选判断。

据此实现了唯一针对性修正版：[site-read-ic-packed-transform.py](/Volumes/zhitai/workspace/lite-engine/txiki.js/build/lto-benchmark/quickjs-hotpaths/site-read-ic-packed-transform.py)。它复用 v1 全部 handler、资格、warmup、miss 退休和 canonical 策略，只把 shape 的身份移入原有 hashed 字节与 padding，以 `uint32_t is_hashed:1; uint32_t read_ic_layout_id:31;` 表示；runtime 计数和 site 的两路 ID 缩到32位，饱和上限固定为 0x7fffffff。达到上限仍永久拒绝新 ID，既有 ID 保留，不改变 ABA 证明。C 代码只通过 bitfield 成员访问，不依赖其位序或大小端，也不把这个内部布局写入公有字节码。

独立代理实际测得 arm64 布局如下。日志中原 `hash` 列实际是 `hash_table` offset，下面按其真实含义列出：

| 结构/offset | integrated-final | v1 | packed 诊断版 |
|---|---:|---:|---:|
| sizeof(JSShape) | 56 | 64 | 56 |
| offsetof(JSShape, proto) | 48 | 48 | 48 |
| offsetof(JSShape, hash_table) | 56 | 64 | 56 |
| sizeof(JSFunctionBytecode) | 128 | 144 | 144 |
| sizeof(JSRuntime) | 5592 | 5608 | 5672（含诊断计数） |
| sizeof(JSReadICSite) | — | 32 | 24 |
| sizeof(JSReadICPlan) | — | 24 | 24 |
| offsetof(JSReadICPlan, sites) | — | 24 | 20 |

packed 的 flexible-array offset=20 而 sizeof(plan)=24 是对齐尾 padding；分配仍用 sizeof(plan)，canonical 放在 sites+count 后，因此多留4字节，不是越界。runtime 的诊断计数也占空间，不能直接用 5672 与正式版本做内存差额。

packed 编译、专项、命中验证与计划分配失败注入均通过。扩充专项诊断 plans=22、sites=57、累计 bytes=3007、layouts=24、calls=1851、hits=1413、misses=438、unhashed=10、retired=12；缓存命中实际发生且验证当前 descriptor。只有 shape/entry 的体积税被缩减，function 仍多16字节。

两种 debugger patch 应用顺序的纯源码 replay 已通过，不当成真实 inspector 构建结果。独立测试代理在对象构造诊断结束后顺序运行本版双预热与六对，避免主动重叠计分；外部系统负载仍需由原始结果判断。若本版没有稳定正增量，本轮结束 IC 候选，不再改准入、way 数或扩大 fusion 支持来继续筛选。
