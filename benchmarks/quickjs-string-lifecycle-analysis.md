# QuickJS 字符串生命周期与重复内容处理

2026-11-10，基线 engine `fa509604`（已保留正则起始锚点裁剪），`build/lto-benchmark/integrated-regexp-anchor/tjs`；macOS arm64 / Apple M4，Release + ThinLTO，PGO/inspector OFF。所有诊断和原型保存在独立 `string-lifecycle-analysis` 目录，不改正式 patch 或 submodule gitlink。

## 1. 先测 Map/Set 内容 hash 的生命周期复用

`map_hash_key` 对 flat string 每次调用 `hash_string(str, 0)`；find、add 和 Map resize 都会调用它。已有四字符 hash 展开四对中位 -2.10%、Map hash 扩散四对 -0.90%，因此本轮不再改 hash 算法，而是检查重复扫描是否值得建立对象内内容缓存。

诊断在 JSString 的 64-bit padding 加一个只用于计数的 seen bitset：通用字符串分配、atom 复制和无描述 symbol 分配初始化；原地 concat 清零；原对象转 atom 不改内容，因此保留 bitset。它不保存裸指针、不跨对象地址复用，不涉及 ABA。分别记录 Map seed0 和通用 hash 的 seed0/seed1/seed2 是否在当前内容生命周期重复出现；其它 seed 只计总量，不声称可复用。计数按 normal/slice/indirect、8/16-bit、atom/非 atom 分类。只用于本次单 JS 线程 WTB 诊断，未做多 runtime 并发计数设计。

完整 WTB 17 项诊断 exit 0。原始记录见 [string-lifecycle-diagnostic.log](results/string-lifecycle-diagnostic.log)，结构化汇总见 [string-lifecycle-diagnostic.json](results/string-lifecycle-diagnostic.json)，构建指纹见 [string-lifecycle-evidence.json](results/string-lifecycle-evidence.json)。插桩运行的吞吐与迭代次数不同于正式计分；以下是实际执行量，不是时间占比或收益预测。

| Map flat string 类型 | hash 次数 | 同对象/同内容重复 | 扫描 bytes | 可避免重复 bytes |
|---|---:|---:|---:|---:|
| normal 8-bit，非 atom | 427,491 | 312,061 | 3,844,451 | 2,714,314 |
| normal 8-bit，atom | 80,333 | 78,425 | 632,511 | 616,468 |
| normal 16-bit，非 atom | 3,152 | 2,888 | 6,304 | 5,776 |
| normal 16-bit，atom | 4 | 2 | 8 | 4 |
| slice / indirect | 0 | 0 | 0 | 0 |
| **总计** | **510,980** | **393,376** | **4,483,274** | **3,336,562** |

Map rope hash 也是 0 次。Map flat key 平均仅 8.77 bytes，重复调用率 **76.98%**、可避免字节率 **74.42%**，但绝对可避免扫描只有 **3.34 MB / 本次完整 WTB**。通用 `hash_string` 共 3,662,614 次、36,470,788 bytes；主要来自 atom seed1 的 3,151,349 次/31,982,985 bytes，其同对象重复只有 4,846,215 bytes，不能把不同新字符串的相同内容算成对象内 cache 命中。这里的通用计数只覆盖名为 `hash_string` 的 JSString 入口；`JS_NewAtomLen` 直接对外部字节串调用 `hash_string8`、BigInt 对 limb 调用它的工作不包含在这个总数中，不能称为引擎全部 hash 时间或字节数。

原地 append 发生 231,774 次，此前在这些对象上出现上述 hash 的次数为 0；这只是本次 WTB 的观测，不是可以省略失效协议的证明。

**决定：关闭 Map-only hash cache 候选。** 复用率看似高，但扫描量不足以优先承担所有字符串初始化/生命周期维护及新快路分支。没有构建生产候选、没有 Map-cache 六对吞吐结果，也不声称它已被实测证明为负收益。这个决定是实验优先级的量级门槛。

## 2. 布局与内容修改审计

复制当前 JSString 定义作[独立原生布局检查](experiments/string-lifecycle-layout.c)，[实际输出](results/string-lifecycle-layout.log)：原结构大小 24，`hash_next` offset 8，`first_weak_ref` offset 16；插入 uint32 缓存后大小仍 24，缓存 offset 12，weak-ref 仍 offset 16。诊断实际引擎也报告同样布局。因此当前 64-bit Release 的 4-byte gap 确实可用，而不是根据声明猜测。32-bit 不承诺该 padding；若继续这个方案，必须编译期关闭缓存或显式接受结构变大。本轮没有 32-bit 运行验证。

| 生命周期路径 | 内容与潜在缓存规则 |
|---|---|
| `js_alloc_string_rt` | 分配后内容未初始化，缓存必须无效；包括普通字符串、StringBuffer、slice 描述符等。 |
| `__JS_NewAtom` 直接接管非 atom 字符串 | 内容不变，只改变 atom 元数据；独立 seed0 内容缓存可保留。不能直接复用现有 28-bit、seed=atom_type 的 atom hash。 |
| `__JS_NewAtom` 复制已有 atom / 无描述 symbol | 新对象必须初始化；atom0 通过 mallocz 初始化。 |
| StringBuffer widen/realloc/put/end | 修改尚未发布的独占新字符串；没有先可观察地使用同一字符串作 Map key 再继续 builder 的路径。 |
| substring slice | 新 slice 持有父字符串引用，父内容不可因 unique-ref concat 原地修改；slice 的内容 hash 与父整串不同，不能继承父缓存。 |
| `JS_ConcatString2` | 已发布字符串在 unique-ref 且空间足够时原地追加，是必须失效的路径；不能仅凭 JS 字符串不可变就略过。 |
| `toWellFormed` / UTF 转换 / Base64 / Hex | 先分配独立输出，再填内容或修改长度，返回前完成；不是修改已发布输入。 |
| regexp INDIRECT 字符串 | 先新建指针载体后设 kind/len，作为内部 regex 字节码；本次 Map/hash 诊断没有命中。 |
| rope linearization | 产出独立 flat 字符串；共享 rope 将 left 改为 flat、right 改为空串缓存。没有重新写原 flat 内容。 |

若将来实现完整 hash cache，应保存 seed0 的完整 32-bit hash，保证 atom/非 atom、8/16-bit 相同字符串最终 Map hash 完全相同；0 哨兵需要明确不缓存 hash=0 等策略。存储必须属于对象生命周期，不能以无 owning 关系的全局 `JSString*` 地址表代替。

## 3. 从 flatten 计数转向借用字符串视图

同一次诊断记录：新 flatten **358 次 / 8,548,384 code units**，已缓存 rope 的返回 **9,408,939 次**。后者的逻辑输入长度和为 1,813,698,498,835 code units，但原实现已经 memoize，**这些字节没有被再次复制**。不能据此宣称需要重写 rope 以避免 TB 级拷贝。

仍存在的工作是：每次 native 字符串 builtin 把 cached rope 再经过 `JS_ToStringCheckObject → JS_ToStringInternal → js_linearize_string_rope` 取得临时 flat JSValue，增加一次 flat 引用，再在 builtin 末尾释放。`charCodeAt` 还调用索引转换 helper，即使参数已经是 int32。

下一独立原型仅改变 `js_string_charCodeAt` 自身 body：已确定实际 builtin、已进入原 native C frame 后，对 flat 字符串或已经缓存 flat 的 rope，加上 int32 索引，借用已有字符串根直接读 code unit；其余走完整原路径。不会在所有 native call 的 dispatcher 增加 callee 身份分支，不混其它 string builtin，也不跳过现有调用/poll/stack 检查。

准入要求 rope.left 和 rope.right 均为 flat string 且 right.len 为 0，显式验证 left tag；未缓存 rope 保持原 flatten。借用期间无分配、无 coercion、无 callback，接收者由当前 native call 的调用者持有，cached rope 持有其 flat child，slice 持有父字符串；`string_get` 仍处理 normal/slice/indirect。索引无符号 bounds 判断对负 int32 返回 NaN，避免更改 UTF-16 code-unit 语义。

### 3.1 实际准入频率

额外完整 WTB 17 项插桩运行记录在 [charCodeAt 诊断日志](results/string-lifecycle-charcode-diagnostic.log)。这次直接在真实 builtin body 入口联合检查 receiver 和 arg0，不把两个独立边际计数相乘：

| 接收者 | arg0 是 int32 | 其它参数类型/缺省 |
|---|---:|---:|
| flat string | 13,192,469 | 85 |
| 已缓存 flat 的 rope | 7,453,127 | 0 |
| 尚未缓存的 rope | 20 | 0 |
| 其它 receiver | 0 | 0 |

总调用 20,645,701，快路准入 20,645,596（**99.99949%**）；其中 20,643,292 次索引范围内、2,304 次越界。准入视图在本次 WTB 全是 normal 字符串，slice/indirect 为 0。后两者的正确性由专门构造的 native fixture 验证，不能说 WTB 覆盖了它们。

### 3.2 原型与正确性

独立候选转换脚本为 [string-lifecycle-charcode-transform.py](experiments/string-lifecycle-charcode-transform.py)。计分前 quickjs.c SHA-256 为 `11eca481e7473212d99fbd3ec03a7241a7ad68ba64d7249f3ec2c5582dc5fff9`；没有加入生命周期诊断字段/计数器或 Map cache。

[JS fixture](experiments/string-lifecycle-charcode-correctness.js) 在基线与候选均通过 **4,940 个值矩阵 cases**，另外验证 omitted/float/undefined/null/bool/string 索引、对象接收者、coercion 顺序、异常身份、nullish/Symbol receiver、BigInt/Symbol index、方法替换、保存原方法、bind、Proxy interception。矩阵包含 UTF-16 code units、astral 和孤立代理项、空串、长字符串与子串。

[原生 fixture](experiments/string-lifecycle-charcode-native.c) 将候选与从原基线逐字复制的原 charCodeAt body 编译进同一 TU，对比 **102,052 个视图/索引 cases**。显式构造 normal、16-bit、slice、indirect、atom、未缓存 rope 与已缓存 rope，并验证 receiver 与 cached-flat child 的最终 refcount 不变。`-fsanitize=address,undefined -DFORCE_GC_AT_MALLOC` 运行 exit 0，无 sanitizer 报告，见 [原生结果](results/string-lifecycle-charcode-native.log)。

原生测试还在相同编译 JS 代码上经真实 native call 分别安装原 body 与候选 body，重置同一 interrupt budget：正常运行各 **15 次** callback，要求中断时各在第 **2 次** callback 抛异常。该结果验证本原型保留原调用 checkpoint；它没有实现跨 native call 的 producer/value-flow 优化，不能认领其它路线减少调用帧的收益。

两个归档 C fixture（layout/native）随后按仓库 clang-format 格式化，原稿与格式稿分别保留 SHA-256。归档 native-build 脚本读取 `benchmarks/experiments/string-lifecycle-charcode-native.c` 格式稿重编译，再次通过同样 102,052 cases、ASAN+UBSAN+forcedGC 和 15/2 poll 检查，见 [格式稿复验日志](results/string-lifecycle-charcode-native-formatted.log)。正式单功能 [test-string-charcode-at.js](../tests/test-string-charcode-at.js) 已在原基线/候选各运行通过，未修改正式 engine patch。

### 3.3 计时状态

正确性通过后，Terra 在主代理安排的 CPU 独占窗口完成相对 `fa509604` 的 AB/BA 交替六对完整 WTB；每个二进制先预热一遍。原始数据见 [string-lifecycle-charcode.json](results/string-lifecycle-charcode.json)，平台/源码/二进制/套件指纹见 [metadata](results/string-lifecycle-charcode-metadata.json)。

总分配对中位 **+0.5362%**、**6/6 胜**。第六对基线降至 3.67，产生 +2.45% 的偏高配对值；前五对仅 **+0.27%～+0.54%**，去掉第六对的配对中位仍是 **+0.5362%**。六对均值约 +0.81% 被最后一对抬高，不作为典型收益。WTB 输出只有两位小数，这个收益较小，必须与输出量化及有限样本一起理解。

| 轮次 | 基线 runs/s | 候选 runs/s | 配对变化 |
|---|---:|---:|---:|
| 1 | 3.74 | 3.76 | +0.5348% |
| 2 | 3.72 | 3.74 | +0.5376% |
| 3 | 3.73 | 3.74 | +0.2681% |
| 4 | 3.73 | 3.75 | +0.5362% |
| 5 | 3.73 | 3.75 | +0.5362% |
| 6 | 3.67 | 3.76 | +2.4523% |

| 子项 | 六对配对中位变化 |
|---|---:|
| acorn | +1.09% |
| babel | -0.27% |
| babel-minify | -0.30% |
| babylon | +0.74% |
| buble | +1.10% |
| chai | +0.30% |
| espree | +1.17% |
| esprima | +1.80% |
| jshint | -0.22% |
| lebab | +0.13% |
| postcss | +0.96% |
| prepack | +0.68% |
| prettier | +0.38% |
| source-map | +0.15% |
| terser | +1.13% |
| typescript | +1.68% |
| uglify-js | +0.63% |

首轮结果显示该 body-only 原型的小幅增量，但主代理要求独立复测后才决定保留：随后在 fa509604 基线、body-only 与独立 field-chain-outline 候选之间安排六轮全部排列，避免小收益仅来自一次短窗口。独立复测已完成，结果单列在下一节。没有跨 native call，也不减少已经消除的 flatten 拷贝。源码保持纯 C、没有新增字节码或缓存生命周期，其他平台仍未运行验证。



### 3.4 独立三方六排列复测

第一次六对结束后，重新预热，并对同一 fa509604 基线、同一个 body-only 二进制及独立 field-chain-outline 二进制执行三方 **六轮全部排列**，每轮包含三个完整 WTB，仍由主代理保证 CPU 独占。源码/二进制/套件指纹未改变，见 [第二轮 metadata](results/body-outline-triad-metadata.json)；[原始结果](results/body-outline-triad.json)、[汇总](results/body-outline-triad-summary.json) 独立保存，没有与首轮混算。

body-only 相对基线的配对中位 **+0.8032%**，均值 **+0.6712%**，**6/6 胜**。它复现了首轮的小幅正收益，两次独立窗口均全六对正，但不把首轮第六对 +2.45% 当作典型，也不宣称大幅架构倍率。

| 子项 | 独立复测配对中位变化 |
|---|---:|
| acorn | +1.09% |
| babel | +0.80% |
| babel-minify | -0.69% |
| babylon | +0.55% |
| buble | +0.55% |
| chai | +0.30% |
| espree | +1.17% |
| esprima | +1.81% |
| jshint | +1.31% |
| lebab | -0.25% |
| postcss | +0.95% |
| prepack | +0.86% |
| prettier | +0.19% |
| source-map | +0.00% |
| terser | +1.17% |
| typescript | +1.23% |
| uglify-js | +0.32% |

最差子项 babel-minify -0.69%，其余多数小幅正。另一个 outline 候选的负收益不计入 body-only 的贡献，也不推断组合收益。主代理根据两次独立窗口均六对全胜、最大复测子项回退 -0.69%，**正式决定保留 body-only** 并进行集成。本报告负责单独候选证据，正式集成后的全套验证见下文。

正式 `tests/test-string-charcode-at.js` 使用 `tjs:assert`，已经 `node node_modules/eslint/bin/eslint.js --no-ignore ... --fix` 整理，再强制 `--no-ignore` 检查 exit 0；格式化后的同一文件再次在 fa509604 基线及 body 候选运行 exit 0。原未格式化 fixture 和正式测试各自的 hash 分开保留，计分源码与二进制未变。强制 lint、最终测试文件 hash、两二进制 exit code 和 native 复验状态存于 [validation](results/string-lifecycle-charcode-validation.json)。

## 4. 独立重放入口

以下脚本以已存在且未改写的 `build/lto-benchmark/integrated-regexp-anchor` 为基线，输出均隔离到 `build/lto-benchmark/string-lifecycle-analysis`：

```sh
python3 benchmarks/experiments/string-lifecycle-diagnostic-transform.py
python3 benchmarks/experiments/string-lifecycle-build.py string-lifecycle-diagnostic
python3 benchmarks/experiments/string-lifecycle-charcode-diagnostic.py
python3 benchmarks/experiments/string-lifecycle-build.py string-lifecycle-charcode-diagnostic
python3 benchmarks/experiments/string-lifecycle-charcode-transform.py
python3 benchmarks/experiments/string-lifecycle-build.py string-lifecycle-charcode
python3 benchmarks/experiments/string-lifecycle-native-build.py
build/lto-benchmark/string-lifecycle-analysis/string-lifecycle-charcode/string-lifecycle-native
build/lto-benchmark/string-lifecycle-analysis/string-lifecycle-charcode/tjs run benchmarks/experiments/string-lifecycle-charcode-correctness.js
```

WTB 通过此前固定 classic-script wrapper 读取 `/private/tmp/txiki-webtooling-src.Xpz6vi/dist/cli.js`；诊断不能和正式计时混用。build 脚本读取基线 `compile_commands.json` 的真实 quickjs.c entry，替换为变体 source/object，在独立 archive 中替换 `quickjs.c.o` 后按原 ThinLTO 链接命令重链，不修改共享 build 或正式源码。


## 5. 正式集成验证

独立构建 `integrated-charcode-body` 完成 Release + ThinLTO 的 inspector OFF→ON→OFF 切换；六项优化回归每个配置均通过，ON 模式 inspector 测试通过。重复 configure 保持 quickjs.c 与 libregexp.c 和计分候选逐字一致；实际编译命令包含 ThinLTO，无 PGO。

完整套件使用 `env -u TJS_HOME build/lto-benchmark/integrated-charcode-body/tjs test tests`：507 通过、1 个 inspector 跳过、1 个既有 `test-tcp-connect-abort.js` 失败（对照同样失败）。详情见 [集成证据](results/string-lifecycle-charcode-integration.json)。这不代表其他平台已验证，也不以此前累计分数加本次增量推断新累计收益。
