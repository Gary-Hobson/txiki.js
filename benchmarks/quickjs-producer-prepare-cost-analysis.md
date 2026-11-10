# Producer preparation cost control

Status: build and mode validation passed. Work was paused when remaining account
quota reached 59%, before performance measurement. No score or process-time result yet. This experiment does not change the formal
engine or reopen the rejected producer candidate.

The preceding [same-binary control](quickjs-producer-region-runtime-gate-analysis.md)
found enabled/canonical +0.40724%, while canonical/body was −1.47024%. This test
asks whether preparation scans, optional plans and their lifecycle account for
a meaningful part of the shared-framework deficit.

## Implementation boundary

The [pure transform](experiments/producer-prepare-cost-transform.py) consumes the
runtime-gated source `e4536f2d…`. It adds a runtime-tail boolean and initializes it
once in `JS_NewRuntime2` from `QJS_PRODUCER_PREPARE_SKIP`. At the first line of
producer preparation, the skip mode returns before eligibility scans or optional
allocation. The prepared mode retains all existing scanning, plan filling and
lifecycle behavior. Both externally set `QJS_PRODUCER_REGION_CANONICAL=1`, so
neither installs or executes private producer entries.

Both modes use one physical executable and have identical JSRuntime and
JSFunctionBytecode layouts, handler code and serializer/free/memory checks. The
additional boolean is after existing runtime fields, avoiding changes to their
offsets. No environment lookup or mode check is added to a call handler.

Candidate source SHA-256:
`92134de0bf67d868a4999aecf0ab033c57acd905c661800e53038457af64bd8e`.
[Design metadata](results/producer-prepare-cost-design.json) records input and
transform hashes, exact environments and staged acceptance criteria.

## Validation and execution stages

The local runner is
`build/lto-benchmark/quickjs-hotpaths/producer-prepare-cost-runner.py`.

1. Build the score and separate diagnostic binaries against the retained body's
   patched archive/link configuration.
2. Run producer semantic and bytecode fixtures in prepared and skipped modes.
   The fixed diagnostic must show equal prepare-call counts, one real 36-byte
   plan in prepared mode, no scans or plans in skipped mode, and zero private
   entries in both modes. Each skipped prepare must be counted explicitly.
3. After this gate and a fresh account-limit check, run one warmup per mode and
   all six permutations of prepared/skipped/body. Record every observation and
   real executable/source/suite hashes immediately, without wrapper hashes.
4. Report WTB geometric-mean scores separately from monotonic full-process
   elapsed seconds. Before interpreting a recovery, the new prepared mode must
   reproduce a deficit against the separate body anchor.

Implementation-stage and pre-build quota checks both reported **40% used**.
At any stage boundary, **41% used or more requires immediate stop**. Partial
logs are retained and must not be described as a completed six-round result.

## Interpretation limits specified before measurement

A meaningful skipped/prepared improvement would support the combined cost of
scanning, optional allocation and plan lifetime under the shared binary layout.
Little recovery would argue against that group being the dominant cause of the
reported framework deficit. Neither outcome separates the new function pointer
from code generation or allocator effects.

Compiler and reader allocate the function header, constant pool, variable and
closure definitions, and bytecode in one combined allocation. In the body source,
these paths are near lines 37570 and 40283; `cpool_offset` is aligned from
`sizeof(*b)`. Consequently, adding a function pointer can change more than the
fixed header size: constant-pool/bytecode relative addresses and allocator size
classes can move. The pointer and combined allocation layout remain present in
both modes, so this test cannot isolate or dismiss those effects.

Much of preparation can occur while loading/compiling the bundle before the
benchmark's timed callbacks. A process-startup improvement without a runs/s
improvement would not explain a WTB score regression. The separate body anchor
still has different structures and compiled code; it is not a pure estimate of
one field's cost. No throughput conclusion is available until measurements are
complete.


## Preserved validation at pause

The score and diagnostic builds completed. Existing logs confirm that producer
semantics and compiler/reader bytecode fixtures passed in both modes. The fixed
[diagnostic](results/producer-prepare-cost-diagnostic.json) records 1,959 prepare
calls per mode: prepared allocated one 36-byte plan; skipped counted all 1,959
bypasses and performed zero scans or plan allocations. Both modes had zero
private entries. The [validation logs](results/producer-prepare-cost-validation-logs.json)
are archived verbatim from the completed runs; source and existing executable
hashes are recorded in the design metadata.

No WTB scoring run was started. Committing these preserved artifacts does not
resume the optimization goal or its quota-check automation. The rejected producer
remains outside the production patch; this experiment has no performance verdict.
