# S1 independent source review

## Scope and result

I read all four files in `natives/src`, all three sittings of the guide, and the corresponding `NativeMachine`, `NativeLibrary`, `Wire`, `LuaJITMachine`, and `CCLuaJITCommon` sources. I also inspected `NativeLoader.cleanupStale`, the native re-entrancy tests, and LuaJIT implementation details relevant to stack movement, allocation accounting, and traceback inspection.

No new defect was identified in the normal S1 JNI exception handling, wire decoding, allocator accounting, or Java handle ownership protocol. This is a source-review conclusion, not proof that all failure modes are absent.

The principal additional finding is that the busy-path message race is C undefined behavior even without a concurrent free. The guide acknowledges the race but understates its consequences. A second item promotes the already-mentioned gap before `NativeMachine.create`'s `try` into an explicit resource-leak finding. Previously reported constructor, logging, decoder-classification, and loader-cleanup problems are confirmed below rather than presented as new discoveries.

Severity here includes reachability: **Low** means an unusual failure or violation of the normal ownership contract is required. The native race could have severe consequences if reached, but I found no route through the current Java locking protocol. There is no demonstrated normal-use native crash in this review.

## Findings

### R1 — Low: rejecting a concurrent C call still races on ordinary C objects

**Status:** Additional consequence of an acknowledged problem; overlaps guide finding 14 but does not require `cm_free`.

**Checklist:** C7, C11; the claimed second line of defence in the C header.

**Locations:** `natives/src/machine.c:237` (`machine_enter`), `:144` (`msg_set`), `:159` (`msg_append`), `:869` (`append_traceback`); `natives/src/cclj_machine.h:16`.

**Trigger:** Two threads call `cm_load`/`cm_resume` on the same live machine. One owns `busy`; the other loses the CAS while the owner writes its result or traceback. Neither thread needs to free the machine.

**Mechanism:** The loser calls `msg_setz(m, "machine is busy")` at line 242 without owning the flag. That writes `msg`, `msg_len`, and `msg_tb` through ordinary stores. The owner accesses the same objects through ordinary reads and writes. The failed CAS uses relaxed ordering and does not acquire ownership; making that CAS acquire would not serialize these accesses either. There is no happens-before relation protecting the message.

The header's statement that every write keeps the length within `CM_MSG_MAX`, and the guide's discussion of an offset becoming larger than the length, do not make the race defined. Bounds reasoning for `msg_append` depends on exclusive access to `msg_len` between the guard, destination calculation, and increment. The C memory model does not promise that the outcome is limited to garbled diagnostics.

**Impact and reachability:** Undefined behavior in the native process when the fallback is exercised concurrently. The existing Java monitor/running protocol prevents this during ordinary use. Same-thread re-entry from a host call is a different case: it is sequential and does not itself create this data race. The already-known concurrent-free use-after-free remains a separate lifetime problem.

**Evidence and limits:** Confirmed by source access analysis. No ThreadSanitizer run or concrete out-of-bounds write was reproduced. The existing cross-thread test at `natives/tests/machine/machine_test.c:2162` blocks the owner in a host callback until the losing calls finish (gate release at line 2178), so it deliberately avoids concurrent message writes. Its passing result does not validate this fallback under a real message race.

**Suggested resolution:** A failed acquisition should return status without mutating the shared machine. If per-call busy diagnostics are required, return them through storage owned by the caller or a separate synchronized mechanism. Document separately that callers must ensure lifetime before accessing even `magic` or `busy`; an internal flag cannot make a freed pointer safe. Changing only the header would acknowledge the limitation but would not make concurrent rejection safe.

**Verification after a fix:** Exercise rejected calls while the owner finishes an error/traceback, with lifetime held until all callers join; use a race detector where supported. Retain the same-thread re-entry tests, adjusting any expectation of a shared busy message if that contract changes.

### R2 — Low: native allocation precedes the cleanup region in `NativeMachine.create`

**Status:** Already noted in the guide's L5 table, but absent from its numbered code findings.

**Checklist:** L5, including the claim that every creation failure frees the native state.

**Location:** `common/src/main/java/io/github/mindbound/cclj/natives/NativeMachine.java:86` through the `try` at line 90.

**Trigger:** `nNew` succeeds, then `LIVE.incrementAndGet()` fails on method entry with `StackOverflowError` before execution enters the `try`. Whether this occurs depends on JVM execution/inlining and remaining stack; it is not an ordinary heap-allocation failure of the counter operation.

**Mechanism:** `h` is allocated at line 86. The only cleanup is the `finally` at lines 101–103. The counter call at line 88 lies outside that region, so a failure there skips `free(h)`. No `NativeMachine` has been returned and no other object owns this handle.

**Impact:** The native state leaks. `LuaJITMachine` may catch and translate the error, but that cannot recover the lost raw handle. The `live()`/`leaked()` counters are not reliable evidence for this path: failure can occur before registration, and `leaked()` counts only refused frees.

**Evidence and limits:** Source-confirmed cleanup gap; no stack-exhaustion reproduction was attempted. The guide already identifies the same trigger, so this is not claimed as a newly discovered execution path.

**Suggested resolution:** Establish the cleanup region immediately after successful native allocation. Keep counter registration and cleanup accounting consistent if registration itself fails; simply moving the counter call inside the existing `try` could decrement an unregistered allocation on failure.

**Verification after a fix:** A controlled failure between allocation and publication must free exactly once and leave both accounting counters consistent. Do not rely solely on counters which the failing operation updates.

### R3 — Low: constructor failures can escape the exception types CC:T expects

**Status:** Confirms guide findings 1 and 2.

**Checklist:** L5 / F15.

**Location:** `common/src/main/java/io/github/mindbound/cclj/computer/LuaJITMachine.java:75`, `:94`, and `:95`–`:98`.

**Triggers:** An unchecked exception from `bios.readAllBytes()`; a checked exception thrown by addon code without a Java `throws` declaration; or a failure while the catch block logs or formats the original throwable.

**Mechanism:** The BIOS read precedes the `try`. The general handler catches only `RuntimeException | Error`. Its own `LOG.error` and string concatenation run outside any protecting handler, and concatenation can invoke a throwable's overridden `toString()`. The preceding `NativeMachineException` handler also constructs a new exception outside the protected body.

**Impact:** Instead of the expected `IOException` or `MachineException`, another throwable escapes the constructor. Under the CC:T creation behavior described in F15, this can leave the computer unable to complete its normal boot-failure handling. These paths normally occur before allocation or after `create` has cleaned up; R2 is the distinct native leak gap.

**Evidence and limits:** Control-flow confirmation, not a new addon/CC:T runtime reproduction. Allocation failure while constructing the replacement exception cannot be eliminated by catch ordering alone.

**Suggested resolution:** Include the BIOS read in the conversion boundary while preserving `IOException`; cover unexpected throwables from addon calls; guard logging and throwable rendering. Keep the documented ordering and exception contract aligned with the resulting behavior.

**Verification after a fix:** Use a throwing BIOS stream, an addon failure with a throwing `toString`, and an undeclared checked exception. Assert the externally observed exception type and native allocation balance.

### R4 — Low: diagnostic logging can replace the intended machine result

**Status:** Confirms guide finding 3, with a qualification to its blanket statement that nothing leaks.

**Checklist:** L4 and the outcome mapping.

**Location:** `common/src/main/java/io/github/mindbound/cclj/computer/LuaJITMachine.java:121`, `:135`, and `:138`.

**Trigger:** Logging or formatting fails, for example under memory pressure while escaping a long error message.

**Mechanism:** Error logging and `forLog` are unguarded. In the `catch (Throwable)` branch, logging at line 121 also precedes `close()` at line 122. A throw from logging skips that close.

**Impact:** A throwable replaces the intended `MachineResult.error`; the caller can show its generic abort diagnostic instead of the Lua error. For terminal native statuses, `NativeMachine.resume` has already freed the handle. That reasoning does not cover every exception path: `new Outcome` at `NativeMachine.java:150` can fail after a successful YIELD, leaving the machine open, and a subsequent failure at the log call skips this method's explicit close. External lifecycle cleanup may still close it; I did not establish a permanent leak through CC:T. The guide's unconditional “Nothing leaks” is stronger than this method alone proves.

**Evidence and limits:** Source-confirmed ordering; no injected logger or double-OOM reproduction.

**Suggested resolution:** Make logging best effort and ensure it cannot prevent required close or result mapping. Preserve the useful bounded escaping of messages.

**Verification after a fix:** With a throwing logger, terminal errors should retain their result and an exception path with an open machine should still close it. Test long-message escaping separately from logger failure.

### R5 — Low: decoder corruption is classified as lifecycle misuse

**Status:** Confirms guide finding 4; defensive path only.

**Checklist:** L4 / outcome mapping.

**Locations:** `common/src/main/java/io/github/mindbound/cclj/natives/Wire.java:104`; `common/src/main/java/io/github/mindbound/cclj/natives/NativeMachine.java:131`; `common/src/main/java/io/github/mindbound/cclj/computer/LuaJITMachine.java:118`.

**Trigger:** A malformed `nMessage` array, such as fewer than four bytes or a traceback offset beyond its text.

**Mechanism:** `Wire.message` uses `IllegalStateException` for malformed data. `handleEvent` rethrows every exception of that type to preserve the closed/already-running contract, so decoder failure bypasses internal-error mapping too.

**Impact:** The caller sees a thrown exception rather than the intended internal-error result. The current JNI message encoder does not produce such malformed arrays, so this is not a current normal-input failure. The terminal resume cleanup still runs.

**Suggested resolution:** Distinguish decoder failures from lifecycle precondition failures and update M1's mapping accordingly; the implementation currently follows M1's broad rethrow instruction.

**Verification after a fix:** Inject malformed message data and verify internal-error mapping; independently retain `IllegalStateException` for closed and concurrent resume calls.

### R6 — Low: best-effort cleanup can permanently disable an already-loaded library

**Status:** Confirms guide finding 5; inspected because sitting 3 includes `NativeLibrary`.

**Locations:** `common/src/main/java/io/github/mindbound/cclj/natives/NativeLoader.java:89`–`:105`; `common/src/main/java/io/github/mindbound/cclj/natives/NativeLibrary.java:42`–`:54`.

**Trigger:** Directory iteration fails during stale-cache cleanup after `System.load` succeeds.

**Mechanism:** Directory iteration can throw unchecked `DirectoryIteratorException`. Both cleanup handlers catch only `IOException`. The exception escapes to `ensureLoaded`, which caches `RuntimeException` as a permanent failure before setting `loaded`.

**Impact:** Later boots select the fallback despite the native library having loaded successfully; restarting the JVM is needed to discard the cached failure. This requires a filesystem failure during cleanup, not ordinary missing cache files handled by the existing catches.

**Suggested resolution:** Contain relevant cleanup failures within the best-effort cleanup boundary. Avoid turning successful loading into a sticky failure because stale files could not be removed.

**Verification after a fix:** Simulate an iteration failure after successful loading; ABI checking and successful publication should still occur, and subsequent calls should reuse that success.

## Documentation and coverage findings

These are existing guide findings confirmed by reading the implementation, not additional runtime defects:

- **Tag definitions (guide 10):** `cclj_machine.h:11` and `machine.c:9` define PROTECTED too narrowly. Tagged helpers run inside protected bodies without being those bodies; `push_value` is a helper, not directly called by Lua. Explain the two tags on `raise_text`.
- **Lua API inventory (guide 11–13):** The header's coroutine list omits `lua_checkstack`, used at `machine.c:728`. M1's table omits `lua_rawget`, `lua_status`, `lua_typename`, and the allocating number-copy conversion. These omissions do not reveal an unprotected raise in the reviewed implementation. The status description also omits existing message-reset and missing-error-value branches.
- **C API contracts (guide 15–19):** State the intentional INTERNAL result for non-null events; NULL behavior of `cm_message` and `cm_free`; the oversized-request use of E_JNI; allocation *attempts* counted by the test hook; and that `wire_rd.left` includes the byte at `p`.
- **Reference/counter/init wording (guide 20–22):** `JniHost.self` belongs to the native invocation's frame; LIVE decrements on a refused free; a repeated `init` changes the cache path before its early return.
- **Safe publication (guide 6):** `NativeMachine.handle` is initialized outside its monitor, but the current `LuaJITMachine.vm` final-field publication makes this an ownership documentation point, not a demonstrated defect in current use. I would not request locking the constructor solely on the evidence in this review.
- **Tests (guide 7–9, 23):** The listed gaps remain relevant, especially JNI allocation failures, the ABI mismatch branch, event/result refusals at the `LuaJITMachine` layer, and log truncation. A missing test alone is not evidence that the corresponding implementation is wrong. The additional busy-message race coverage gap is described under R1.

`cclj.c` is the M0 self-test, not the S1 machine bridge. Its deliberate light userdata, library opening, and `NewStringUTF` use must not be treated as violations of the file-specific S1 restrictions. I read its report bounds, protected-call boundaries, and interrupt-thread lifetime handling; no additional actionable defect was established. Its introductory “machine bridge comes later” wording is stale but has no runtime effect.

## Checklist disposition

| Checks | Source-review disposition |
|---|---|
| C1 | Raising operations are in protected execution or host-call phases 1/3; tag/table wording needs the existing corrections. |
| C2–C3 | Both cross-thread Lua-stack moves have destination `checkstack(n + 1)`; external coroutine inspections follow the permitted operations. |
| C4 | Host call returns before any phase-3 Lua error; request/reply buffers remain machine-owned across raises. No reachable finalizer source was introduced by the curated library setup. |
| C5 | Growth arithmetic avoids subtraction underflow; accounting starts from LuaJIT's count; original allocator is restored before close. |
| C6 | Readers check remaining lengths before advancement, counts are byte-bounded, method totals are capped, and trailing bytes are rejected. Java readers check before narrowing lengths. |
| C7 | Acquisition/release paths balance for owners, but rejected concurrent callers write unprotected message state: R1. Lifetime protection still belongs to Java/caller. |
| C8 | S1 cpcall userdata is the machine; load/resume bodies drop the argument before running Lua. Number conversion does not run user Lua. |
| C9 | DEAD is not resumed; load failure is terminal; coroutine state is checked before resume. |
| C10 | No release-build mutable statics or extra direct printing in machine.c; debug/test checks are conditional. This is source inspection, not a fresh release-symbol audit. |
| C11 | Sequential traceback generation is bounded and allocation-free, with separate frame-count/debug records. Concurrent access invalidates its synchronization premise: R1. |
| J1–J6 | No new issue found: throwing calls are checked/cleared, each successful upcall frame is popped, returned locals are managed, conversions are bounded, and env/self are invocation-local. |
| L1–L3 | Handle take-and-zero is synchronized; native calls/message reads retain ownership; native/host calls occur outside the instance monitor. R2 is the pre-publication cleanup exception. |
| L4 | Normal returned results are non-null; lifecycle misuse throws. R4/R5 qualify exceptional result mapping. |
| L5 | Normal creation failures inside the cleanup region free the handle; R2/R3 qualify the universal cleanup/type guarantees. |

## Validation and limits

Executed `bash natives/tests/machine-test.sh windows-x86_64`. It rebuilt the native test executable from the current sources, reused the script-validated assert/API-check LuaJIT tree, and passed **166 checks, 0 failed**. The final accounting check reported **0 of 7,684 checks differ**; **1,102 tracebacks** had no tested address/builtin-name or stack-top violation. This is the Windows run without ASan/UBSan, as specified by the test script.

An initial direct execution of the pre-existing test binary also passed, but the rebuilt run is the relevant validation. No new tests or repro source files were added. This review did not rerun the JNI/Java suites, Linux tests, race sanitizers, release lint, or the full game. Existing G1 logs are prior evidence, not new results from this review. Rare stack, logging, and filesystem failure findings above are based on source control flow and are explicitly not claimed as reproduced failures.

The known S1 omissions—events, aborts, C-stack limiting, tables, and yielding callbacks—are scheduled slices in M1 and were not relabeled as new defects. No source or checklist changes are included with this report.
