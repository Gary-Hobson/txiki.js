# 对象布局原型的 ASAN / GC-stress 复核

这些是手动复现 fixture，包含正式对照也会崩溃的故障注入；不属于正常 `tests/` 套件。引擎版本为 QuickJS `7b5f588e`，对照是 `retained-ef801eaa`，候选是最终 shape 共享修正版。准确源码 SHA-256、编译 flags 及状态见 [验证元数据](../results/object-layout-sanitizer-validation.json)。

可使用独立生成的引擎 C 文件复现，`object_repro_source` 指向对照或候选的完整 C 源码。下例故意运行已知失败 fixture：

```sh
object_repro_source=/absolute/path/to/quickjs.c
clang -std=gnu11 -O1 -g -DNDEBUG -D_GNU_SOURCE \
  -DFORCE_GC_AT_MALLOC -DQJS_OBJECT_LAYOUT_DIAGNOSTICS \
  -funsigned-char -fsanitize=address -fno-omit-frame-pointer \
  -Ideps/quickjs \
  "$object_repro_source" deps/quickjs/dtoa.c \
  deps/quickjs/libregexp.c deps/quickjs/libunicode.c \
  benchmarks/experiments/object-layout-asan-oom-repro-closure.c \
  -o /tmp/object-layout-asan-oom-repro
ASAN_OPTIONS=halt_on_error=1 /tmp/object-layout-asan-oom-repro
```

`-DNDEBUG` 保持原型启用：该 QuickJS 版本的 debug 模式自动开启 dumps，原型会因 dumps 回退。ASAN 与 forced GC 仍启用；`FORCE_GC_AT_MALLOC` 同时使小对象直接交给底层分配器，让 ASAN 能逐块识别已释放内存。不要用 `plans=0` 的通过结果声称验证了优化路径。

| fixture | 覆盖与实际结果 |
|---|---|
| `object-layout-asan-oom-repro-closure.c` | 在整个 script 执行期间注入失败。对照和候选都在 closure 创建的 `resize_properties → OOM error creation → JS_RunGC` 路径崩溃。 |
| `object-layout-asan-oom-repro-array.c` | 先安装 closure，再注入 literal initializer。对照和候选都在 initializer 的 Array 构造失败时，于 `JS_NewObjectFromShape` 写 length 属性的 NULL 地址崩溃。 |
| `object-layout-asan-oom-data.c` | 先安装 closure，initializer 只创建普通数据对象和长字符串。候选探测180个位置、实际注入148次，148次均捕获JS异常并恢复副作用顺序/后续构造/GC；剩余32个位置没有发生分配。对照仍被其逐字段 `resize_properties → OOM error creation → GC` 旧路径阻断。 |
| `object-layout-asan-native.c` | 实际优化的直线 literal 中调用C，执行GC、活跃序列化、原型换置、递归及跨realm检查。候选22 callbacks通过，plans4/sites4、objects17、直接字段45、共享命中2。可传输出字节码路径及另一引擎字节码路径，检查双向读取；两版输出逐字一致。 |
| `object-layout-asan-cycles.c` | 50次建立并解除 `shape → prototype → function → bytecode → shape` 强环，GC后function/shape/object数量逐次精确恢复。候选plans50/shapes50/objects50，ASAN无报告。 |

失败归因依据是相同 allocator、源码及完整失效路径，不是要求两版第N个分配具有相同含义。fixture没有修改引擎去绕开旧缺陷，也没有声称所有 ASAN/OOM/GC-stress 组合通过。原始 baseline/candidate trace 以 `object-layout-asan-oom-*` 命名保存在 `benchmarks/results/`。

Parser-disabled 编译使用相同 flags 追加 `-DQJS_DISABLE_PARSER`，编译候选引擎源文件为 `.o`，已通过；这只是该配置的编译验证。

归档的C副本已按仓库 `.clang-format` 格式化，所有字符串字面量合并后的内容与执行时build目录中的原fixture一致。trace中的源码行号对应执行时未格式化的build原始文件，不能直接作为格式化副本行号。

归档日志仅清除了行尾空格；地址、调用栈与计数保持原样，本机 build 目录仍保留原始输出。
