# Private call-method producer regions

Status: **rejected**. Six paired WTB runs against the retained charCodeAt body
had median **−1.21%**, mean **−1.25%**, and **0/6 wins**. The candidate passed its
JS/native correctness and coverage screening but is not included in the engine.
No further integration validation is needed for this rejected version.

## Evidence and scope

The [same-site producer diagnosis](quickjs-producer-site-analysis.md) observed
10,321,886 successful flat-string/int-index `charCodeAt` calls followed by three
specific suffix shapes. These represented 78.26% of that diagnostic's in-bounds
domain, not 78.26% of execution time or all calls. The histogram aggregates
syntax shapes across sites; a bucket's representative function is not the
source of every observation.

| Suffix after call_method | Observed calls | Stack value retained after branch | Baseline VM dispatches consumed |
| --- | ---: | --- | ---: |
| set_loc, constant, gt, branch | 4,814,812 | none | 3 |
| set_loc, constant, lte, dup, branch | 3,458,114 | boolean | 5 |
| set_loc, dup, constant, strict_eq, branch | 2,048,960 | original code unit | 4 |

The last column accounts for the already retained adjacent comparison/branch
fusion. Its frequency-weighted sum is 39,930,846 main-VM dispatches. This is a
syntactic opportunity estimate: local eligibility and non-target callee tax
were not measured by that earlier diagnostic. Cached-rope producers were also
outside its flat-string domain. New counters distinguish actual successful
consumption, flat and cached-rope producers, failed local guards, domain misses,
and sites permanently retired on their first non-target callee.

The input is the retained object/regexp engine, optionally with the separately
tested charCodeAt body optimization. The body baseline removes ToString/ToInt
and temporary string reference-count traffic while retaining the complete
native call. This experiment must be compared incrementally against that body
version if retained; it does not claim those body savings again.

## What this implementation removes

At a matching site and actual native callee identity, the pure producer avoids
the recursive `JS_CallInternal` entry, class-call indirect dispatch, native
frame publication/restoration, realm switching and the indirect builtin call.
It still performs the call's original interrupt poll and native argument-buffer
stack check. The producer is a borrowed immutable string view, index check and
UTF-16 code-unit load. The post-call region avoids materializing the constant
and comparison operands on the VM stack, rechecking their generic tags, local
dup/free for immediate values, and the suffix's main-VM dispatches. It directly
produces only the surviving stack value, if any.

Costs are a pointer in every bytecode-function structure, compile/read-time
scans, a plan only for functions with matching sites, plan/site lookup at each
private entry, actual-callee/domain guards, and the post-cleanup local guard.
This is not a claim that the dispatch count equals removed CPU branches or
cycles. It also does not prove that native frame overhead dominates the body.
The earlier dispatcher intrinsic and int-region results make screening these
costs necessary.

## Implementation and continuation protocol

The pure transform and correctness fixtures are archived under
`benchmarks/experiments/producer-region-*`. Build logs remain local under
`build/lto-benchmark/quickjs-hotpaths/producer-region*`:

- [producer-region-transform.py](experiments/producer-region-transform.py): pure source transform, no submodule edits.
- [producer-region-correctness.js](experiments/producer-region-correctness.js): three suffix outputs, fallback and excluded-function cases.
- [producer-region-bytecode.js](experiments/producer-region-bytecode.js): canonical round trip and compiler/reader allocation-failure injection.
- [producer-region-native.c](experiments/producer-region-native.c): deterministic poll, active serialization, recursive retirement, local guard and ROM-read tests.
- [producer-region-checkpoint-extra.c](experiments/producer-region-checkpoint-extra.c): finalizer/cleanup, stack-check and foreign-realm tests.

The optional plan records the original call offset, branch offset, local index,
integer constant, comparison and output mode. It owns no JSValue, atom, shape
or borrowed pointer. One non-throwing runtime allocation creates each plan;
allocation failure leaves bytecode unchanged and no JS exception pending.

Only `call_method` with argc=1 and one of the exact suffix shapes is replaced.
Private opcode 253 stores a uint16 site index in the original two-byte argc
operand. Opcode 254/255 belong to the retained object construction plan. The
scanner recognizes their canonical lengths and treats their operands as opaque.
It never indexes a public-opcode table with those private numbers.

The private handler reads its entire three-byte entry and publishes the original
call continuation in `sf->cur_pc` before any call, interrupt or cleanup callback.
All suffix bytes remain canonical. Consequently, restoration of the three-byte
entry cannot strand a paused or reentrant invocation inside overwritten bytes.
An in-flight outer handler can retain a recipe when an inner invocation retires
that same site; the plan remains allocated until its bytecode function is freed.
Production never re-quickens a retired entry.

Compiler and bytecode-reader preparation run after object-plan preparation.
Both zero-initialize the added pointer. Serialization restores each producer
entry in the temporary output copy before object-plan canonical restoration;
the running bytecode buffer stays at a stable address. Destruction restores the
producer entry before the original atom-release walk and frees the plan. Memory
accounting includes each plan's allocation and the enlarged bytecode structure.
Plans contain only scalar data, so no additional GC marking is needed.

The private definition, dispatch-table entry and handler are excluded when the
inspector's `JS_DEBUG_TRACE_DEBUGGER_STMT` macro is present. Preparation is also
disabled under `ENABLE_DUMPS` and `QJS_DISABLE_PRODUCER_REGION`. A static assertion
requires the ordinary opcode count to leave 252 as the default range and 253 as
the private entry. Actual inspector compilation remains a validation gate; a
runtime-disabled feature alone is insufficient proof of opcode compatibility.

## Exact guards and observable checkpoints

Normal bytecode functions only: any own captured-local/argument reference count,
generator/async function, direct eval, dynamic with reference, explicit local or
argument reference creation, or mapped-arguments creation excludes the function.
The suffix cannot contain a TDZ load, property operation, call or another poll.
Its write is to an ordinary local, not an argument or closure slot.

Identity is taken from the actual callee on the original call operand stack:
`JS_CLASS_C_FUNCTION`, generic C calling convention, exact
`js_string_charCodeAt` function pointer, magic=0 and native length=1. Method-name
or prototype assumptions do not substitute for identity. Getters, overrides,
proxies and bound functions therefore retain normal property/call semantics.
A first non-target callee permanently restores the original call entry. A target
callee with a non-int index, boxed receiver, uncached rope or out-of-bounds index
uses the ordinary call path but does not retire the entry.

The pure domain accepts a flat string, or a cached rope whose left child is flat
and right child is the empty string, with an int32 index inside the view. The
operand stack owns the receiver until cleanup, and a cached rope owns its flat
child. No string pointer is used across the call interrupt callback: the view is
reloaded afterward. A defensive post-poll domain miss continues directly through
`js_call_c_function`; it does not repeat the already executed call poll.

The original call poll runs in the caller's context before any native frame
publication. The original native argument-buffer stack-check expression follows.
The exact machine-stack limit boundary can change because the fast path has
fewer C frames; the invariant is that the check still runs at the same semantic
checkpoint and throws before producing or committing the result. The pure body
cannot allocate, coerce, invoke user code or throw after this check, so it needs
no native realm/frame. Domain fallbacks retain the complete native realm and
frame, including realm-specific conversion exceptions.

After a successful producer, cleanup uses the original order: receiver, callee,
then argument. The stack is contracted only after those releases. A callee's
properties can own a custom-class object with a finalizer; therefore the local
guard deliberately runs **after cleanup**, not merely before the call. The old
local must be INT or undefined. Otherwise the result is pushed and execution
resumes at the already-completed call's canonical suffix. It never repeats the
call, conversion, side effects or call poll. No delayed free of an old local is
allowed.

Once the local guard passes, assignment and the int comparison cannot fail.
Only then does the handler write the local, construct the correct surviving
stack value, take the original branch edge, and perform its original interrupt
poll. An interrupt at this point sees the same committed local and stack state
as the ordinary suffix. No earlier checkpoints are skipped or merged.

## Diagnostics and acceptance gates

`QJS_PRODUCER_REGION_DIAGNOSTICS` is disabled for timing. It records scanned
functions, plans/sites, cumulative allocation bytes, current/peak live plan
bytes, private entries, actual targets, non-target retirements, domain misses,
generic calls, fast flat/cached-rope producers, local failures, successful
suffixes by output mode, consumed main-VM dispatches and same-run VM dispatches.
Cumulative allocation bytes must not be described as live or peak memory.

Correctness requires actual hits, not tests that silently use the generic path.
The internal native fixture asserts a real plan and successful cached-rope
entry; the JS fixture must produce all three diagnostic output modes. Tests
exercise call-poll local mutation, finalizer mutation during original callee
cleanup, recursive same-site retirement, active-frame canonical serialization,
ROM bytecode reading, exact normal/abort poll counts, native stack failure,
cross-realm conversion exceptions, unsupported dynamic environments, generators
and async continuations. Plan-allocation failure must actually be injected and
fall back in both compiler and bytecode-reader preparation.

Only after correctness and a same-run WTB coverage/cost diagnosis should a
paired throughput experiment be considered. Excess non-target site/plan tax,
large domain-fallback traffic, or little actual suffix consumption can reject
the candidate without a throughput claim. A high hit count alone is not an
acceptance criterion. Any measured gain must be additional to the retained
charCodeAt body, with the same regexp/object engine and build/link settings.

## Reproducible source boundary

The pure transform applied without source-anchor changes to both inputs:

- regexp/object input SHA-256:
  `4025d0bc28c45b8a68aaccd603b0309cfe53fef70dc37f3e3957f133fdf29d58`;
  producer output `ce69ca5ee29f82aa8f63e40e6dadea2e5b3d5224131b3d0c4f462f73d2d40253`.
- body input SHA-256:
  `11eca481e7473212d99fbd3ec03a7241a7ad68ba64d7249f3ec2c5582dc5fff9`;
  producer output `2fa928c10fefee23d4a7c4e1059d0e72d72b8604cd975593d8c01efb211c467d`.

These hashes identify the initial candidate, not an unperformed benchmark.

## Correctness screening results

Terra built the normal, diagnostic and forced-plan-allocation-failure variants
against `integrated-charcode-body`. Baseline, normal candidate and diagnostic
candidate passed the producer JS suite and the retained object/private-bytecode
regressions. The forced-allocation-failure candidate passed those suites, with
an actual optional-plan failure observed in the producer suite.

The internal native test passed 15 normal interrupt callbacks per 50,000 calls
and aborted on callback 2, matching the same function with canonical entry
restored. It verified active serialization, recursive same-site retirement with
the outer recipe still in flight, actual post-call local guard failure, an
actual cached-rope fast hit, and ROM-flag bytecode reading. The extra native
test verified that callee cleanup finalization precedes the local write, that
a finalizer's replacement object forces a local fallback, that the native stack
check still throws after a call interrupt callback reduces its limit, and that
a foreign builtin's conversion error retains the foreign TypeError prototype.

Two initial fixture defects were corrected before these passes: the cached-rope
fixture needed a second rope owner because this runtime only memoizes flattening
when rope refcount exceeds one; and `JS_NewClassID` requires the runtime argument
in this API. Neither required a change to the candidate transform. These are
fixture corrections, not evidence of candidate failures or throughput results.

The dedicated compile/read round trip also passed on baseline, candidate,
diagnostic and forced-allocation-failure versions. Its compile and read phase
markers each contained an actual optional-plan failure. Actual inspector
compilation was a proposed integration gate, separate from this OFF-only
candidate screening; it was not run after the throughput rejection.

Archived native fixtures were clang-formatted after execution; no semantic edits
were made. [Replay evidence](results/producer-region-replay-evidence.json) records
both executed and archived hashes and verifies that the archived transform
reproduces the exact tested candidate source. No native build was repeated during
the throughput isolation window.

## Same-run coverage result

The [diagnostic record](results/producer-region-diagnostics.json) contains one full
WTB run, with explicit GC statistics flushed before exit. It is not a timed
score. Results:

| Measurement | Count |
| --- | ---: |
| Scanned functions | 68,998 |
| Allocated plans / selected sites | 51 / 52 |
| Cumulative / live / peak plan bytes | 1,856 / 1,856 / 1,856 |
| Private entries / actual target callees | 12,150,351 / 12,150,347 |
| Non-target permanent retirements | 4 |
| Target-domain misses / post-poll defensive misses | 11 / 0 |
| Actual fast flat / cached-rope producers | 10,335,399 / 1,814,937 |
| Post-cleanup local guard failures in WTB | 0 |
| Successful suffixes: none / keep-code / keep-bool | 4,814,812 / 2,763,073 / 4,572,451 |
| Total successful suffixes | 12,150,336 |
| Removed main-VM dispatches | 48,358,983 |
| Executed main-VM dispatches | 4,107,759,691 |

Adding back the consumed suffix dispatches gives a same-run reference count of
4,156,118,674; the reduction is **1.16356%**. This reconstruction is a dispatch
count, not a CPU-time percentage or an independently executed baseline run.
Cached-rope consumption is now directly measured, rather than inferred from the
earlier flat-only producer histogram. The earlier native body test's total
charCodeAt frequency must not be added to these counts.

Only 52 sites bear recurring entry checks and four non-target sites retired.
This removes the earlier uncertainty about an extensive non-target call tax for
these exact suffixes. The universal bytecode-structure pointer and preparation
scans still have costs; plan bytes do not include either the enlarged function
structures or their GC traversal footprint. These are reasons to measure the
complete engine despite the small plan footprint.

The coverage gate passed and justified six paired runs against
`integrated-charcode-body`. The executed metadata records one warmup per binary
(two warmup runs total), followed by six alternating-order pairs. The completed
measurement rejected the candidate, as detailed below.

## Throughput decision and unfinished validation gates

[Raw scores](results/producer-region.json) and
[run/source/binary metadata](results/producer-region-metadata.json) identify the
Release + ThinLTO binaries, unchanged suite, and alternating pair order. Ratios
are computed from each pair's reported geometric-mean score; input scores are
rounded by WTB.

| Pair | Body score | Producer score | Change |
| --- | ---: | ---: | ---: |
| 1 | 3.73 | 3.69 | −1.07% |
| 2 | 3.74 | 3.68 | −1.60% |
| 3 | 3.74 | 3.68 | −1.60% |
| 4 | 3.74 | 3.69 | −1.34% |
| 5 | 3.72 | 3.68 | −1.08% |
| 6 | 3.71 | 3.68 | −0.81% |

The median is −1.2061%, mean −1.2503%, with no winning pair. Fifteen subtests have
negative paired medians; babylon (+1.10%) and jshint (+1.87%) are positive. These
results reject this complete implementation, despite its verified coverage.
They do not establish that the pure producer operation alone is slower, because
structure, preparation and shared interpreter code-generation costs changed too.

The candidate was stopped before these proposed integration gates:

- Real inspector ON/OFF candidate builds and inspector execution.
- Re-execution of the clang-formatted archived native fixtures with ASAN/UBSAN
  and forced GC. The executed native fixtures passed; this stronger validation
  was prepared but not performed.
- `sizeof(JSFunctionBytecode)` measurement and comparable full-WTB GC/memory
  snapshots. Only allocated/live/peak **plan** bytes were measured. The universal
  function-structure pointer cost is not included in the 1,856-byte figure.

No claim of sanitizer cleanliness or inspector validation is made. Prepared
local `producer-region-sanitizer-runner.py` and
`producer-region-memory-transform.py` were not run; they are not measured
candidate changes or prerequisites left open for merging this rejected version.
The source transform and all executed semantic fixtures remain archived for
reproduction.

## Static cost evidence and one falsifiable follow-up design

Read-only `nm`, `otool` and `size` inspection of the scored binaries is recorded
in [static-cost evidence](results/producer-region-static-cost.json):

| Observation | Body | Producer | Difference |
| --- | ---: | ---: | ---: |
| Mach-O __text bytes | 4,087,476 | 4,092,252 | +4,776 |
| JS_CallInternal symbol-to-next-symbol interval | 46,248 | 48,832 | +2,584 |
| JS_CallInternal local stack reservation in prologue | 864 | 880 | +16 |
| Callee-save stack reservation | 96 | 96 | 0 |

This ARM64 build's shared VM code and stack layout changed even though only
52 sites use the new handler. The observation supports investigating a shared
interpreter cost; it does not identify instruction-cache misses, register spills,
branch prediction or any other PMU mechanism. Total executable text grew only
about 0.12%, so a vague whole-binary code-size explanation is insufficient.

The next minimal experiment, **designed but not executed**, is one control
binary with a process-level flag read only at preparation for selected plans:

1. Both modes perform identical eligibility scans, allocate/fill the same plans,
   retain the new bytecode pointer, and retain the same compiled private handler,
   serializer/free/memory code and machine-code layout.
2. The flag controls only the final loop that overwrites call_method with opcode
   253. The off mode leaves canonical entries but retains its plans until normal
   bytecode destruction. It does not use `QJS_DISABLE_PRODUCER_REGION`, which
   would remove preparation/code and permit a different LTO layout.
3. Once prepared, the flag is immutable. Neither mode changes running bytecode
   back and forth; there is no re-quickening protocol or additional call guard.
   Read the environment/control setting once per selected prepare (51 allocations
   in the diagnostic) or cache it once per runtime; do not read it on every call.
4. First verify off-mode private-entry count is zero, on-mode hits are real, and
   both modes have equal plan counts/bytes for an identical fixed fixture. Then
   alternate the two modes using the **same executable hash**. Use the retained
   body binary as an external anchor if measuring total framework cost.

This separates the direction of the selected execution path's net contribution
from its shared code/metadata framework more directly than two separately
compiled variants. If on beats off while both trail the body, the producer path
is beneficial inside an overly costly framework. If on trails off, the selected
execution path itself adds a net cost under the shared layout. If their difference
is unresolved, it does not justify another handler rewrite. Any comparison to the
separate body binary still includes its different layout and structures and is
not an exact attribution to the new pointer or preparation scans. The control
binary's enabled mode must also reproduce the original regression before using
it to explain that regression.

This is a single cost-discrimination experiment, not a new production candidate
or evidence that a gain remains available. No further build or benchmark has
been started for it.
