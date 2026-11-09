# Producer-region cost control

Status: completed. Across six permutations, enabled beat canonical by a paired
median **+0.40724% (6/6 wins)**, while enabled and canonical both trailed the body
anchor: **−1.06809%** and **−1.47024%**, respectively, both **0/6 wins**. This is a
cost-discrimination result, not a production improvement. The
[original producer version](quickjs-producer-region-analysis.md) remains rejected.

The [gate transform](experiments/producer-region-runtime-gate-transform.py) takes
the exact rejected candidate source. Both modes scan the same functions and
allocate/fill the same plans. After publishing a plan, one `getenv` check controls
only whether its original call_method entries are overwritten by private opcode
253. `QJS_PRODUCER_REGION_CANONICAL=1` leaves canonical instructions; an absent
variable enables the producer. Plan statistics, fields, ownership, serialization
and release remain in both modes. There is no flag read in a call handler and no
toggle of already running bytecode.

Unlike a compile-time disable, this preserves one executable, including private
handler and shared interpreter layout. The external environment is fixed for
each process. The WTB workload does not change that variable. A function cannot
be re-quickened after its entry was retired.

The gated source SHA-256 is
`e4536f2d0375f43383170784212a7b50550f0ae5215952c8babb052ca77846cd`,
derived from the measured producer source
`2fa928c10fefee23d4a7c4e1059d0e72d72b8604cd975593d8c01efb211c467d`.
[Design metadata](results/producer-region-runtime-gate-design.json) records the
transform and mode settings. Raw timing and interpretation are recorded below.

The local runner is
`build/lto-benchmark/quickjs-hotpaths/producer-region-runtime-gate-runner.py`.
Authorized commands, in order, are:

```sh
python3 build/lto-benchmark/quickjs-hotpaths/producer-region-runtime-gate-runner.py build
python3 build/lto-benchmark/quickjs-hotpaths/producer-region-runtime-gate-runner.py diagnose
python3 build/lto-benchmark/quickjs-hotpaths/producer-region-runtime-gate-runner.py score --rounds 6 --include-body
```

The build uses the retained body's patched QuickJS source, static archive and
link command. Diagnostic and score builds are separate. The diagnostic gate
runs the producer semantic and bytecode tests in both modes, then uses the same
fixed 100-call fixture to assert equal plan/site/allocation/live-byte counts,
zero private entries in canonical mode and 100 additional successful suffixes
in enabled mode. Startup/automatic-GC snapshots are excluded by comparing the
last two explicit GC snapshots around those calls.

Only after those assertions pass, the score command runs one warmup for each
mode and all six permutations of enabled/canonical/body order. Enabled and
canonical use the same physical executable with the same recorded SHA-256;
there are no wrapper executables. Body is a separate retained-engine anchor.
Metadata records actual binary/source/transform/suite hashes and environment
differences. Every completed observation and raw log is written immediately so
an interrupted run remains auditable. Partial observations are not a completed
six-round result.

Interpretation was specified before measurement:

- Enabled beating canonical would support a beneficial selected execution path
  within this shared framework. If both trail body, the total framework still
  defeats that benefit.
- Enabled trailing canonical would reject the selected execution path under
  the same compiled layout; it would not identify a particular instruction,
  branch predictor, register spill or cache mechanism.
- An unresolved enabled/canonical difference would not support further handler
  tuning. A difference against the separate body engine still mixes metadata,
  preparation and compiled-layout effects.

The enabled gate mode must reproduce the original regression before using this
experiment to explain that regression. The single additional preparation check
can itself change LTO decisions compared with the original candidate, even
though it cannot produce a layout difference between this experiment's two
modes. No result can be generalized into a speedup claim for the rejected
producer version.

## Verified mode boundary

The [fixed-fixture diagnosis](results/producer-region-runtime-gate-diagnostic.json)
passed after the producer and bytecode correctness suites ran in both modes.
Each mode scanned 1,590 functions and allocated one plan/site with 36 bytes,
including identical live and peak plan bytes. Enabled mode had 100 actual hits
and removed 300 main-VM dispatches; canonical mode had zero private entries.
No domain or local-guard failures occurred in this fixed fixture.

[Timing metadata](results/producer-region-runtime-gate-metadata.json) verifies
that enabled and canonical both use actual executable SHA-256
`0ebb347baa319e3d5b28167da4fa9225c9c2ea4b9ae4da23213fe9a663be1635`.
The separate body anchor remains
`3a2bda3b3243225dbb8c25c88245e3b6091af3ca82778be9d811354a11160b6a`.
The runtime-gated source matches the design hash above. These are real binary
hashes, not launch-script hashes. All 18 timed observations completed.

## Completed measurement

[Raw observations](results/producer-region-runtime-gate.json) and
[paired summaries](results/producer-region-runtime-gate-summary.json) record all
six order permutations and 18 timed executions. There was one warmup per mode.

| Round | Enabled | Canonical | Body |
| --- | ---: | ---: | ---: |
| 1 | 3.70 | 3.68 | 3.74 |
| 2 | 3.70 | 3.69 | 3.75 |
| 3 | 3.70 | 3.68 | 3.74 |
| 4 | 3.71 | 3.70 | 3.75 |
| 5 | 3.70 | 3.68 | 3.73 |
| 6 | 3.71 | 3.70 | 3.75 |

| Paired comparison | Median | Mean | Wins |
| --- | ---: | ---: | ---: |
| Enabled / canonical | +0.40724% | +0.406996% | 6/6 |
| Enabled / body | −1.06809% | −1.068332% | 0/6 |
| Canonical / body | −1.47024% | −1.469284% | 0/6 |

The enabled control reproduces the direction and approximate size of the
original producer regression. Within this control binary's identical compiled
layout and data structures, executing the selected producer path is modestly
beneficial. This falsifies the simple explanation that the producer execution
path itself must be a net loss just because the complete candidate is slower.
The canonical control's larger deficit supports a net cost in the common
framework that outweighs that local benefit.

This is not an exact additive decomposition of the median score: ratios combine
multiplicatively per round, and the separate body binary still differs in
structure size, preparation, metadata lifecycle and machine code. The run does
not identify register pressure, I-cache effects, the new pointer, or scanning as
the individual cause. No candidate is retained on the strength of this diagnosis.

## Next minimal framework test — design only

The next useful contrast is **prepared canonical versus unprepared canonical in
one new control binary**. The new mode would skip the entire producer preparation
scan/allocation while retaining the same JSFunctionBytecode field, private handler,
serializer/free checks and compiled VM layout in both modes. The existing
prepared-canonical mode would continue to create and retain plans without
installing private entries. Neither mode would execute the producer handler.

Read a process setting once during runtime creation, cache it in a dedicated
experimental runtime field, then check it at the start of prepare. Both modes
must use the same physical executable and identical runtime/function structure
sizes. This adds no per-call check. Do not implement this as a compile-time
feature removal; that would again mix in code elimination and LTO layout changes.
Validate zero private entries in both modes, real plans in prepared mode, no
plans in unprepared mode, identical fixed-fixture outputs and canonical bytecode.

This asks one falsifiable question: are the scans, optional allocations and their
lifecycle a substantial part of the reported framework deficit? A substantial
recovery in unprepared mode would support that group of costs. Little or no
recovery would reject it as the dominant explanation under the shared layout,
leaving function-structure and VM-code-generation effects unresolved. The
prepared mode must retain the previously observed deficit against body before
using a new binary to explain it.

Measure the reported WTB score and full-process elapsed time separately. Much
of the 68,998-function scanning occurs during bundle compilation, before timed
WTB callbacks, so a startup improvement alone cannot explain a runs/s deficit.
Post-GC `JS_ComputeMemoryUsage` snapshots and actual sizeof measurements can
quantify the allocation effect, but must be diagnostic runs outside score timing.
The universal per-bytecode pointer remains in both modes and cannot be credited
or blamed from this contrast.

Only this design is authorized at this stage. No implementation, new build,
sanitizer run or additional benchmark has been started for it.
