# Fair, Non-Regressing CPU Multi-Threaded Page-Copy Offload for migrate_pages()

## Goal Description

Shivank Garg's RFC v3 "Accelerate page migration with batch copying and hardware offload" (`20250923174752.35701-1-shivankg@amd.com`, co-developed with Zi Yan) introduced an `mtcopy` driver: a CPU-thread-pool that parallelizes `folio_copy()`'s work across a batch of folios (or across chunks of one large folio) during migration, showing up to ~3.4x throughput on measured hardware for large folios. Two concrete objections surfaced in review that were never resolved in the RFC thread and are the reason this hasn't progressed toward mergeable shape:

1. **Busy-system regression** (Byungchul Park, `20251020082800.GA28427@system.software.com` / `20251112021217.GA45963@system.software.com`): `mtcopy`'s worker fan-out uses `system_unbound_wq` — ordinary workqueue kworkers at normal scheduling priority. On an idle system this helps; on a busy system, spawning N extra runnable threads competes with real workload for CPU time and can make things *worse* than today's single-threaded copy, not just fail to help. Byungchul's bar: "at worst, the performance should be same as is."
2. **CPU time charge-back for scheduling fairness** (Zi Yan, `633F4EFC-13A9-40DF-A27D-DBBDD0AF44F3@nvidia.com`): when copy work is farmed out to separate kernel worker threads, the CPU time it consumes is accounted to those workers, not to the task that triggered the migration (a `move_pages()` caller, `kcompactd`, `kswapd`, NUMA balancing, etc.). This makes migration cost partially invisible to standard per-process/per-cgroup CPU accounting (`top`, `ps`, `cpu.stat`), and lets a task's migration-induced CPU consumption escape its own cgroup's CPU quota, since the worker threads run outside that cgroup. Zi Yan has an unreviewed local "hack" for this and has explicitly said the proper fix needs scheduler-maintainer input that hasn't happened yet.

**Chosen mechanism: extend padata rather than build a bespoke worker pool.** The same RFC message records that Zi Yan already replaced mtcopy's hand-rolled workqueue fan-out with `padata_do_multithreaded()` locally, blocked on padata being `__init`-only. Investigation of `kernel/padata.c` confirms padata is the right substrate for three reasons beyond code reuse: it already load-balances chunks across helpers with caller-specified alignment and minimum chunk size; it already supports NUMA-aware helper placement (`job->numa_aware` → `queue_work_node()`); and — most importantly for objection 1 — **it already runs the job on the calling thread in parallel with the helpers** (`kernel/padata.c:498-500`, INV-15), so a fully-starved helper pool degrades to exactly today's single-threaded behavior by construction rather than by benchmark luck.

This plan is scoped to two staged tiers of the deprioritization work (Tier 0 and Tier 1 below), plus the charge-back problem. It explicitly defers a third tier and one latency fix to a Deferred Work section, so the first mergeable step stays small. It does **not** redesign RFC v3's batching/pluggable-migrator architecture (patches 1-5: `migrate_folios_batch_move()`, `MIGRATE_NO_COPY`, the `static_call`-dispatched `struct migrator` framework), nor does it touch the DMA offload driver (`dcbm`, patch 7/9).

### The tiers

- **Tier 0 — make padata usable at runtime.** `padata_do_multithreaded()` and its helpers are `__init`-annotated (`include/linux/padata.h:186,191`; `kernel/padata.c:48,109,135,404,444,452`), so they are freed after boot and unusable from `migrate_pages()`. Verified (INV-14) via `git show 004ed42638f44` (Daniel Jordan, 2020, the commit that introduced multithreaded padata — Zi Yan was Cc'd) that these annotations were present from the first version as a scoping decision reflecting the then-only callers (boot-time struct-page init), not as a correctness barrier. **In scope.**
- **Tier 1 — give padata's multithreaded helpers a dedicated, deprioritized workqueue.** Padata currently queues helper work to the shared `system_dfl_wq` (`kernel/padata.c:493,495`), whose priority it cannot change. A padata-owned `WQ_UNBOUND` workqueue with `attrs->nice = 19`, applied via the existing `apply_workqueue_attrs()` path (honored at worker attach, `kernel/workqueue.c:2874`), requires no changes to workqueue core. Weight arithmetic: nice 0 = 1024, nice 19 = 15 (`sched_prio_to_weight[]`, `kernel/sched/core.c:10606-10615`), i.e. ~68x deprioritization — see INV-18, INV-19. **In scope.**
- **Tier 2 — true `SCHED_IDLE` helpers.** `WEIGHT_IDLEPRIO` = 3 (`kernel/sched/sched.h:2511`), only ~5x below nice 19's weight of 15 (INV-18). Reaching it requires `struct workqueue_attrs` (`include/linux/workqueue.h:148-152`) to carry a scheduling policy, not just `nice` — a workqueue-core change needing maintainer review. **Deferred; see Deferred Work.**

## Acceptance Criteria

- AC-1 (Tier 0): `padata_do_multithreaded()` and its supporting helpers are usable after boot, with the runtime-safety implications of that change explicitly reasoned about rather than assumed.
  - Positive Tests (expected to PASS):
    - The `__init`/`__initdata` annotations are removed from `padata_do_multithreaded()`, `padata_mt_helper()`, `padata_work_alloc_mt()`, `padata_works_free()`, and `last_used_nid` (`kernel/padata.c:48,109,135,404,444,452`), and from the declaration and `!CONFIG_PADATA` stub in `include/linux/padata.h:186,191`. `padata_init()` itself (`kernel/padata.c:1089`) correctly remains `__init`.
    - The `__ref` annotation and its explanatory comment on `padata_work_init()` (`kernel/padata.c:92-99`), which exists solely to silence modpost about `padata_mt_helper()` being `__init` under clang LTO, is removed or updated — it becomes stale once the `__init` it references is gone.
    - A written analysis of the global `padata_works` pool's behavior under concurrent runtime callers accompanies the change: the pool is a fixed `num_possible_cpus()`-entry array allocated once in `padata_init()` and guarded by one spinlock (`kernel/padata.c:34-36,1102-1107`) — see INV-21. `padata_work_alloc()` returns `NULL` when exhausted and `padata_work_alloc_mt()` simply stops allocating (`kernel/padata.c:83-84,113-121`), so contention degrades helper count rather than failing — but this graceful-degradation property must be stated as a deliberate, verified conclusion, since a boot-time-only pool now serves an unbounded number of concurrent runtime callers.
    - Build passes with `CONFIG_PADATA=y` and `CONFIG_PADATA=n`, and with clang LTO if available (the configuration the `__ref` comment cites).
  - Negative Tests (expected to FAIL / be rejected):
    - Removing the annotations without addressing the pool-contention question at all.
    - Leaving the now-stale `__ref` comment in place asserting a constraint that no longer exists.
- AC-2 (Tier 1): padata's multithreaded helpers run on a dedicated, deprioritized workqueue, and this is demonstrated by measurement to satisfy Byungchul's "at worst, same as is" bar on a busy system without giving up the idle-system speedup.
  - Positive Tests (expected to PASS):
    - Padata allocates its own `WQ_UNBOUND` workqueue for multithreaded-job helpers and applies `attrs->nice = 19` to it via `alloc_workqueue_attrs()`/`apply_workqueue_attrs()`; helper work is queued there instead of `system_dfl_wq`, for both the `numa_aware` (`queue_work_node()`) and non-NUMA (`queue_work()`) paths (`kernel/padata.c:486-496`).
    - Existing boot-time callers (`mm/mm_init.c:2143`, `mm/hugetlb.c:3378,3573`) are unaffected in behavior, or any deliberate change to their behavior is called out — note the workqueue must exist by the time the earliest boot caller runs, so its creation point relative to `padata_init()` and to `workqueue_init_early()`/`workqueue_init()` is verified, not assumed.
    - **Busy-system benchmark** (background load pinned across the CPUs the helper pool would use, e.g. `stress-ng --cpu N`) measures *both*: (a) combined system throughput, and (b) **caller-observed wall-clock latency of the migration itself**. Both must be no worse than the same benchmark with the copy pool disabled (single-threaded `folio_mc_copy()` fallback). Measuring (b) and not just (a) is required, not optional — see the Deferred Work section's completion-wait item for the specific mechanism that could regress caller latency even while system throughput looks fine.
    - **Idle-system benchmark** reproduces RFC v3's original mtcopy speedup for large folios (or close to it), confirming nice-19 helpers still get meaningful CPU when nothing else wants it.
  - Negative Tests (expected to FAIL / be rejected):
    - Continuing to queue helper work to `system_dfl_wq` or any other shared workqueue, whose priority padata does not own.
    - Reporting only aggregate system throughput from the busy-system benchmark, without caller-observed migration latency.
    - Tuning a static "safe" helper count as the busy-system mitigation instead of letting the scheduler arbitrate via weight — the whole point of the tiering is that load handling is structural, not guessed.
- AC-3: Migration makes bounded progress even when helpers get little or no CPU.
  - Positive Tests (expected to PASS):
    - The design relies on padata's existing caller-participation (`kernel/padata.c:498-500`: the calling thread runs `padata_mt_helper()` inline, whose `while (job->size > 0)` loop at :411-427 drains chunks until the job is empty) rather than adding a separate fallback path (INV-15). If every helper is starved, the calling thread completes the entire job at its own undiminished priority — structurally identical to today's single-threaded copy.
    - This property is stated explicitly in the commit message and verified by a test that pins the helper pool's CPUs under saturating load and confirms the migration still completes.
    - Confirmed that both nice 19 and `SCHED_IDLE` map to `fair_sched_class` (`__setscheduler_class()`, `kernel/sched/core.c:7572-7586`; `set_load_weight()`'s `task_has_idle_policy()` branch at :1529-1531), i.e. proportional-share with no indefinite starvation — helpers are slowed, never permanently blocked (INV-17). This bounds the worst case for AC-2's caller-latency measurement.
  - Negative Tests (expected to FAIL / be rejected):
    - Adding a redundant bespoke fallback path (timeout-to-single-threaded, work-stealing, etc.) alongside padata's caller participation, which would duplicate a guarantee padata already provides and add a second thing to keep correct.
- AC-4: Helper CPU time is attributed back to the initiating task's cgroup where that is structurally sound, is explicitly *not* attempted where it is not, and — for the part CFS structurally cannot do — the gap is **demonstrated with a working prototype and measured evidence**, then turned into a concrete proposal for a minimal in-kernel primitive. This AC does **not** require landing a scheduler-core change; it requires earning the right to ask for one. Zi Yan's account of the RFC thread is that a bare request for help drew silence (DEC-2); the strategy here is to replace "we think there is a fairness problem, please advise" with "here is a measured fairness violation under CFS, here is a working scheduler that fixes it, and here is the minimal thing CFS would need to do the same." AC-4 decomposes into four parts with deliberately different dispositions. **Parts (a), (b) and (c) are in scope; part (d) — any change to `kernel/sched/` — is gated as TODO-4 and must not begin until (c) is complete *and* accepted upstream** (repository-owner decision; see DEC-3).

  **(a) DO — cgroup `cpu.stat` attribution.** Charge helper execution time to the initiating task's cgroup for statistics purposes.
  - Positive Tests (expected to PASS):
    - Helper runtime is charged to the initiator's cgroup via `__cgroup_account_cputime(cgrp, delta_exec)` (`include/linux/cgroup.h:817`, `kernel/cgroup/rstat.c:630`), which takes a **cgroup pointer directly** rather than deriving one from `current` — see INV-3.
    - The approach is documented as following the existing runner/donor precedent in `update_se()` (`kernel/sched/fair.c:1355-1394`), where the kernel already routes cgroup CPU accounting to a task other than the one physically executing, with the in-tree comment "cgroup time is always accounted against the donor" — see INV-2.
    - Measured: a migration-heavy workload inside a cgroup shows that cgroup's `cpu.stat` `usage_usec` increase reflect the helper work done on its behalf, versus the same workload with the copy pool disabled.
    - **No double-counting**, and the reason is stated explicitly rather than assumed: `cgroup_account_cputime()` guards its rstat charge with `if (cgroup_parent(cgrp))`, so a kworker in the **root** cgroup contributes nothing to the rstat tree today, and root's own `cpu.stat` is computed separately by `root_cgroup_cputime()` from global kcpustat rather than from rstat (INV-29). The explicit charge therefore fills a genuine hole rather than duplicating an existing one, and rstat propagation up the initiator's ancestors is correct.
    - **The cgroup is captured with the RCU-safe try-get pattern, not a bare `cgroup_get()`.** `task->cgroups` is RCU-protected and `css_get()`'s own kernel-doc states *"The caller must already have a reference"* (`include/linux/cgroup_refcnt.h:2-13`), so `cgroup_get(task_dfl_cgroup(current))` on a freshly-read pointer is unsafe. The in-tree pattern for stashing `current`'s default cgroup is `rcu_read_lock()` plus a `cgroup_tryget(cset->dfl_cgrp)` retry loop, as `cgroup_sk_alloc()` does (`kernel/cgroup/cgroup.c:7401-7414`) — see INV-34. Released with `cgroup_put()` at job completion. A reference is needed because being blocked in `wait_for_completion()` prevents the initiator from *exiting* but not from being **migrated to another cgroup** mid-job.
    - **Each helper charges before incrementing `ps->nworks_fini`.** Otherwise the last helper can `complete()`, the initiator wakes and drops the reference, and a helper still executing its charge dereferences a freed cgroup. (Alternatively each helper takes its own reference — but ordering is cheaper and simpler.)
    - **Both `usage_usec` and the user/system split are charged**: `__cgroup_account_cputime(cgrp, delta)` for the authoritative total, *and* `__cgroup_account_cputime_field(cgrp, CPUTIME_SYSTEM, delta)` for the split — mirroring the scheduler, which likewise calls the two separately. Charging only the former does not leave `user_usec`/`system_usec` unchanged (they are *derived* from the total by `cputime_adjust()` on read); it silently distributes the added kernel time across them in the cgroup's **pre-existing user:system ratio**, so pure system time can be reported largely as user time. Total right, split wrong — see INV-35.
    - **Measurement is per-helper-per-job, not per-chunk.** `task_sched_runtime()` on a running task misses its lockless fast path and takes `task_rq_lock()` (INV-30); twice per helper per job is negligible, twice per chunk inside a copy loop is not.
    - The measured delta covers the helper's whole invocation (including padata bookkeeping and lock waits), not only `folio_copy()`. This is documented as deliberate — it is time genuinely spent on the job — rather than left as an unexamined approximation.
    - A root-cgroup assertion in the helper guards INV-29's no-double-count argument — but read under `rcu_read_lock()`, not as a bare `task_dfl_cgroup(current)`. Note this is now closer to an assertion than a tripwire: workqueue workers are created with `kthread_bind_mask()`, which sets `PF_NO_SETAFFINITY`, and cgroup migration explicitly refuses such tasks with `-EINVAL` — so padata's kworkers **cannot** be moved out of root (INV-33). The assertion documents an invariant the cgroup code enforces rather than one merely observed to hold.
    - The caller-participation path is **not** instrumented — the initiator running its own chunks is already billed to itself natively (INV-15) — and the mechanism for skipping it is explicit, not left to prose. `padata_mt_helper()` runs on the initiator as well as the helpers, and `struct padata_work` stores no caller/helper flag, so the job state stashes the initiator's `task_struct *` and the helper compares `current` against it (INV-37).
    - The whole feature is `CONFIG_CGROUPS`-guarded, and the direct `__cgroup_*()` calls reproduce the wrapper's `if (cgroup_parent(cgrp))` test explicitly, since the raw forms do not apply it (INV-36).
  - Negative Tests (expected to FAIL / be rejected):
    - Deriving the target cgroup from `current` inside the helper (that is the bug being fixed, not the fix).
    - Capturing the cgroup with a bare `cgroup_get()` on a freshly-read `task_dfl_cgroup()`, or without RCU protection (INV-34).
    - Charging after incrementing `ps->nworks_fini`, which races the initiator's `cgroup_put()`.
    - Charging only `__cgroup_account_cputime()` and assuming the user/system split is unaffected (INV-35).
    - Instrumenting `padata_mt_helper()` unconditionally, which silently charges the inline caller a second time (INV-37).
    - Calling the raw `__cgroup_*()` forms without a `cgroup_parent()` guard, or without `CONFIG_CGROUPS` protection (INV-36).
    - Calling `task_sched_runtime()` per chunk rather than per helper invocation.
    - Charging the caller-participation portion, which would double-count it against the initiator.
    - Implementing the `current->cpu_charge_cgrp` redirect variant instead (see the Feasibility sketch): it is exact and robust to a non-root helper cgroup, but adds a `task_struct` field, puts a load and branch in the scheduler's accounting hot path, carries a stale-redirect leak hazard across kworker reuse, and touches `include/linux/cgroup.h` — at minimum a judgment call against DEC-3's "no kernel sched changes" gate. It is the documented upgrade path, not the first implementation.

  **(b) DON'T — per-task `sum_exec_runtime` inflation.** Do not attempt to make `/proc/<pid>/stat`, `top`, `ps`, or `getrusage()` reflect helper time on the initiating task.
  - Positive Tests (expected to PASS):
    - The plan and commit message state explicitly that per-task visibility is *not* addressed, and why, citing: (i) that naively adding to `utime`/`stime` is a no-op because `cputime_adjust()` rescales them as a ratio of the authoritative `sum_exec_runtime` (INV-1), so the only lever is inflating `se.sum_exec_runtime` itself; (ii) that even proxy-exec — the kernel's own precedent for cross-task attribution — deliberately sends per-task `sum_exec_runtime` and thread-group time to the *running* task, not the donor (INV-4), so there is no in-tree precedent for redirecting it; and (iii) that `se.sum_exec_runtime` feeds POSIX CPU timers/`RLIMIT_CPU`, NUMA-balancing scan heuristics, and `RLIMIT_RTTIME` (INV-7), so inflating it has user-visible side effects well beyond cosmetic accuracy in `top`.
  - Negative Tests (expected to FAIL / be rejected):
    - Any patch that writes to `p->utime`/`p->stime` to represent helper time (mechanically ineffective, per INV-1).
    - Any patch that inflates `p->se.sum_exec_runtime` without separately justifying each side effect enumerated in INV-7.

  **(c) DEMONSTRATE — prove the gap and the fix with a throwaway sched_ext prototype.** Build a minimal BPF scheduler whose *only* purpose is to produce evidence; it is explicitly **not** proposed for merge, and nothing in Tier 0/Tier 1 or AC-4(a) may depend on it (suggested by Andrea Righi; feasibility investigated as INV-9 through INV-13).
  - Positive Tests (expected to PASS):
    - A minimal `sched_ext` scheduler implements `cgroup_set_bandwidth` (`kernel/sched/ext/internal.h:684-703`) plus helper→initiator charge-back, exploiting the fact that scx delegates cgroup bandwidth enforcement wholly to BPF and implements no CFS-style throttling of its own (INV-10), and that scx covers kworkers by default (INV-11).
    - A prototype-only kernel→BPF association channel exists — a tracepoint around `padata_mt_helper()` carrying `(current, initiator)` for BPF to record in task-local storage (INV-12). Because this is throwaway scaffolding rather than proposed ABI, the bar is "sufficient for the experiment," not "fit to commit."
    - **The measured gap is produced and is the actual deliverable**: a migration-heavy workload confined to a cgroup with a set `cpu.max` demonstrably *exceeds* its quota under stock CFS (helper time escapes the hierarchy entirely), and demonstrably *respects* it under the scx prototype. Without this number the exercise has failed regardless of how good the prototype is.
    - **Enforcement is scoped to quota (`cpu.max`) only.** The prototype records how N concurrent helpers charging one initiator's *budget* behaves — the throttling pattern in practice, and whether it is sane — since that is the question a CFS-side design must also answer (Gap 4b, INV-26). Weight-based 1:N fairness (Gap 4c) is an **explicit non-goal**: sched_ext can express it via globally-ordered vtime (INV-32) but CFS structurally cannot, so demonstrating it would produce evidence for a request AC-4(d) is forbidden from making.
    - **Work order inside this part is fixed** (repository-owner decision): enforcement first, `cpu.stat` last. Concretely — (c.1) association channel and helper→initiator charge-back in BPF state; (c.2) `cgroup_set_bandwidth` plus refusing to dispatch over-budget helpers; (c.3) the gap measurement; and only then (c.4) making `cpu.stat` reflect helper time under scx. (c.4) is last because it is the only sub-part that cannot be done in BPF at all: under sched_ext, `rq->donor` and `rq->curr` are the same union member (INV-27) and no kfunc writes cgroup cputime (INV-28), so it requires a new `scx_bpf_cgroup_account_cputime()`-style kfunc — i.e. a kernel change, which shares TODO-4's gating. Everything (c) needs to prove is provable without it.
    - The gap measurement (c.3) is reported and the part considered demonstrable **without** (c.4) — statistics accuracy under scx is not a precondition for showing that the enforcement semantics work.
  - Negative Tests (expected to FAIL / be rejected):
    - Proposing the scx scheduler itself for merge, or letting any in-scope AC acquire a dependency on it.
    - Investing in prototype polish (tuning, generality, additional scx ops) beyond what the gap measurement requires — the artifact is the number, not the scheduler.
    - Starting (c.4) before (c.1)-(c.3) are done, or letting (c.4)'s kernel-side requirement block the measurement.
    - Demonstrating weight-based 1:N fairness (Gap 4c), or letting the prototype drift toward a global-vtime fairness model — impressive, but unportable to CFS and therefore evidence for an unaskable request (INV-32).
    - Conflating (c.4) with AC-4(a): they are different code paths for different builds. AC-4(a) is padata/mm-side, scheduler-agnostic, needs no kfunc, and is **not** deferred (DEC-3).

  **(d) PROPOSE — deferred, gated behind (c)'s upstream acceptance.** The three-piece in-kernel primitive (`cpu_charge_begin()`/`cpu_charge_end()`, redirected charging in `update_curr()`, and a `cpu_charge_over_budget()` query) and its full justification are specified in "The Enforcement Gap" section and tracked as **TODO-4**. When it is eventually written, it must not present pieces 2 and 3 as thin wrappers over existing helpers: both require a cross-CPU hierarchy and locking design that the demonstrator exists to settle. No work on it begins until the AC-4(c) demonstrator is complete *and* accepted by upstream maintainers. Rationale: it is the only part of this plan that touches `kernel/sched/`, it is the hardest to get accepted, and its entire persuasive weight comes from (c)'s measurement — proposing it earlier spends credibility that (c) has not yet earned.
  - Positive Tests (expected to PASS):
    - No patch in this plan's series modifies `kernel/sched/`.
    - "The Enforcement Gap" section remains the written specification of the eventual ask, kept current as (c) produces findings, so that no analysis is lost while the work is parked.
  - Negative Tests (expected to FAIL / be rejected):
    - Any `kernel/sched/` change landing, or being posted upstream, before (c) is complete and accepted.
    - Deleting or letting the Enforcement Gap analysis go stale on the grounds that the work is deferred.

- AC-5: Integrates with RFC v3's existing architecture without redesigning it.
  - Positive Tests (expected to PASS):
    - `copy_page_lists_mt()`'s external signature and its registration via `start_offloading(&cpu_migrator)` are unchanged; only its internal dispatch mechanism changes (from hand-rolled `system_unbound_wq` fan-out plus `flush_work()` to a `padata_do_multithreaded()` call).
    - No changes to `migrate_folios_batch_move()`, `MIGRATE_NO_COPY`, or the `static_call`-based migrator dispatch.
    - RFC v3 patch 9/9 ("mtcopy: spread threads across die for testing") is understood to no longer apply, per Zi Yan's own note in `633F4EFC-13A9-40DF-A27D-DBBDD0AF44F3@nvidia.com`; whether its intent is served by padata's `job->numa_aware` or is genuinely lost is stated explicitly.
  - Negative Tests (expected to FAIL / be rejected):
    - Any change to the pluggable-migrator framework's external contract — that framework is considered settled by the RFC thread (see Jonathan Cameron's review, `20251002121009.00005899@huawei.com`), and re-litigating it is out of scope.

## Path Boundaries

### Upper Bound (Maximum Acceptable Scope)

AC-1, AC-2, AC-3, AC-5, and AC-4(a)/(b)/(c) implemented and benchmarked — including AC-4(c)'s throwaway sched_ext demonstrator through (c.3) and its measured CFS-quota-violation number. **AC-4(d) and AC-4(c.4) are outside even the Upper Bound**: both touch `kernel/sched/` and are gated on the demonstrator being accepted upstream (TODO-4, DEC-3).

### Lower Bound (Minimum Acceptable Scope)

AC-1, AC-2, AC-3, and AC-5 — Tier 0 plus Tier 1 plus the measurement that shows whether that combination clears Byungchul's bar. AC-4 may land as parts (a) and (b) only — the cgroup `cpu.stat` attribution plus the explicit non-goal — with (c)'s demonstrator and (d)'s proposal following separately, since their value is evidence for an upstream conversation rather than code in this series.

### Allowed Choices

- Can use: padata's existing `struct padata_mt_job` interface as-is, or a minimal extension to it (e.g. a flag or nice field selecting the deprioritized workqueue) if boot-time callers should keep using `system_dfl_wq` — that sub-choice is DEC-1.
- Cannot use: any change to workqueue core (`struct workqueue_attrs`, `alloc_workqueue()` flags, worker-attach scheduling setup) — that is Tier 2, deferred; a bespoke `kthread_create()` worker pool parallel to padata; landing new scheduler-core primitives unilaterally without scheduler-maintainer sign-off (AC-4(d) is a *propose*, not *land*, milestone — DEC-2). Note the sched_ext demonstrator of AC-4(c) is explicitly *permitted* and is not a scheduler-core change: it is out-of-tree BPF plus a prototype tracepoint, proposed for merge by nobody; wholesale reuse of `CONFIG_SCHED_PROXY_EXEC`'s donor mechanism as-is.

## Deferred Work (explicit TODOs, out of scope for this plan)

Both items below are deliberately deferred, not overlooked. Each should be revisited with data from AC-2's benchmarks rather than on principle.

- **TODO-1: Tier 2 — true `SCHED_IDLE` helpers.** Requires `struct workqueue_attrs` to carry a scheduling policy alongside `nice`, then applying it at worker attach next to the existing `set_user_nice()` (`kernel/workqueue.c:2874`) via `sched_setattr_nocheck()` (`EXPORT_SYMBOL_GPL`, with in-tree precedent for setting a kworker's policy at `drivers/cpufreq/cppc_cpufreq.c:240`; INV-20). `SCHED_IDLE` is a valid policy for this path (`valid_policy()`, `kernel/sched/sched.h:218-222`). Touch points identified: the struct and a `WQ_IDLE` flag in `include/linux/workqueue.h`; `copy_workqueue_attrs()` (`kernel/workqueue.c:4783`), `wqattrs_hash()` (:4814), `wqattrs_equal()` (:4828), the apply point (:2874), and pool/attrs defaults (:7951, :8042, :8050). Two design questions need a workqueue maintainer's opinion: (a) workqueue uses `nice == HIGHPRI_NICE_LEVEL` as a proxy for "is this a highpri pool" in six places (:1239, :1254, :3776, :3808, :6370, :6381), which a policy field must not muddy; (b) rescuer threads (`set_user_nice(current, RESCUER_NICE_LEVEL)`, :3588) exist to guarantee forward progress under memory pressure and almost certainly must *not* inherit `SCHED_IDLE`. **Trigger for revisiting: AC-2's busy-system benchmark showing nice 19 is insufficient.** Given nice 19 is only ~5x weaker than `SCHED_IDLE` in weight terms (15 vs 3, against 1024 for nice 0), it may well prove sufficient.
- **TODO-2: the completion-wait latency hazard** (INV-16, bounded by INV-17)**.** `padata_do_multithreaded()` waits on `wait_for_completion(&ps.completion)` (`kernel/padata.c:503`), and that completion fires only when `ps.nworks_fini == ps.nworks` (:429-434). `ps.nworks` counts the caller *plus* every allocated helper work item (`padata_work_alloc_mt()` starts its counter at 1 "because the current task participates in the job", :113-115, and returns that count). So each queued helper must be scheduled at least once — merely to observe `job->size == 0`, increment the counter, and return — before the caller is released. **This means the caller can drain the entire job itself via caller-participation and still block in `TASK_UNINTERRUPTIBLE` waiting for helpers that have not yet run.** The structural issue is present at Tier 1, not only at Tier 2 (see the note below on why it is nonetheless expected to be tolerable there), and is present in today's boot-time-only code too, where it is harmless because workers are nice 0 and the system is idle. Fix directions, none chosen: cancel or flush still-unstarted helper work once `job->size` reaches 0; or restructure the completion condition to "job drained and no helper *currently executing*" rather than "every helper has run." **Trigger for revisiting: AC-2's caller-observed-latency measurement showing a regression** — which is precisely why AC-2 requires measuring caller latency separately from system throughput.

  Why this is expected to be tolerable at Tier 0+1 but is measured rather than assumed: both nice 19 and `SCHED_IDLE` are `fair_sched_class` (`kernel/sched/core.c:7572-7586`), i.e. proportional-share with no indefinite starvation, so a queued helper is delayed but never permanently blocked, and once scheduled it returns almost immediately (the job is already drained). At nice 19 the expected delay is on the order of normal scheduling latency under load. That is very likely fine — but "very likely fine" is a hypothesis about a path whose entire purpose is avoiding busy-system regressions, so AC-2 measures it instead of asserting it.

- **TODO-4: the in-kernel scheduler primitive (AC-4(d)), gated on AC-4(c)'s upstream acceptance.** The three-piece ask — `cpu_charge_begin()`/`cpu_charge_end()`, redirected charging inside `update_curr()`, and a `cpu_charge_over_budget()` query over the existing `cfs_rq_throttled()` — is fully specified in "The Enforcement Gap" section and is **not** to be started, prototyped in-tree, or posted upstream until the sched_ext demonstrator is both complete and accepted by upstream maintainers (repository-owner decision, DEC-3). Also gated here: the `scx_bpf_cgroup_account_cputime()`-style kfunc that AC-4(c.4) would need, since it is likewise a kernel change. **Trigger for revisiting: AC-4(c) accepted upstream.** Keeping the analysis written down while the work is parked is deliberate — the reasoning is the expensive part and it should not have to be re-derived.

- **TODO-3: ~~sched_ext as a deferred fallback~~ — superseded, now AC-4(c).** An earlier revision of this plan filed sched_ext as deferred work on the grounds that it only helps where a BPF scheduler is loaded, does nothing for the statistics half, needs new kernel→BPF ABI, and would give mtcopy two divergent fairness behaviors. Those objections are all objections to *shipping* it. Once its role is understood as a throwaway demonstrator whose deliverable is a measurement — not a shipping mechanism — three of the four evaporate: nobody deploys it, so divergent behavior and BPF-scheduler availability are irrelevant, and the association tracepoint is prototype scaffolding rather than committed ABI. The one that survives is real but narrow: sched_ext genuinely cannot help the statistics half (INV-9), so AC-4(a) stands independently and must not acquire a dependency on it. Retained here only so the reversal is on the record rather than silently rewritten.

## The Enforcement Gap: analysis and upstream path

This section is the analytical core behind AC-4(c) and AC-4(d). It exists so the demonstrator is aimed at a specific ask rather than built speculatively.

### The four gaps

Attributing helper CPU time to the initiator needs four things. The kernel has all four — but only under a condition mtcopy cannot satisfy (INV-25).

- **Gap 1 — Association.** `rq->donor` is established solely by `pick_next_task()` plus `->is_blocked` (`kernel/sched/core.c:7148-7156`). There is no caller-driven API to declare "this task is executing on behalf of T/C." Mutex-blocking is the only trigger.
- **Gap 2 — Native `cpu.stat`.** Follows Gap 1 entirely: `update_se()` already routes cgroup time to the donor (INV-2). Solve association and this closes itself. Genuinely small.
- **Gap 3 — Enforcement without borrowed context.** *The real gap.* Proxy-exec's enforcement is **emergent from exclusivity** — the runner runs only because the donor was picked, so dequeuing the donor stops the runner for free (INV-25). A padata helper is an independently-schedulable kworker with its own entity, cfs_rq and weight, so throttling the initiator's cgroup does not reach it.
- **Gap 4 — 1:N with a *running* donor.** Proxy-exec is strictly 1:1 with a *blocked* donor. Here one initiator has N concurrent helpers and is itself running via caller-participation (INV-15). That configuration has no representation in the current model (INV-6). It decomposes into three parts with very different answers, and conflating them is the main way this analysis goes wrong:
  - **4a — plumbing (N runners naming one payer).** A single-field, linear-chain limitation (INV-6, INV-25). Real under CFS; trivially absent under sched_ext, where the association is a map entry (INV-31).
  - **4b — quota semantics (`cpu.max`).** *Not actually a gap.* Quota is denominated in absolute microseconds and therefore composes across CPUs by construction — which is exactly what `cfs_b->quota` plus per-CPU `runtime_remaining` already does. "N helpers consumed X µs" is well-formed against a budget under either scheduler. The consequence is worth stating plainly though: a cgroup capped at 1 CPU that fans out to 8 helpers exhausts its period budget in roughly 1/8 of a period and is throttled, so **parallel copy buys a tightly-capped cgroup essentially nothing**. That is what a quota means, not a defect — but it bounds who this feature helps.
  - **4c — weight semantics (`cpu.weight`).** The genuinely hard part, and the one **CFS structurally cannot express**. A weight is a *per-runqueue ratio*: on each `cfs_rq` tasks receive CPU proportional to weight ÷ sum-of-weights **on that runqueue**. If the initiator sits on CPU 0 while its helpers burn CPUs 1-7 in the root cgroup, nothing about the initiator's weight on CPU 0 can encode "and I am also consuming seven other CPUs"; deflating it merely slows the initiator locally, and the units do not match. Weight does not compose across CPUs the way quota does. sched_ext *can* express it (INV-32) — but only by not being CFS, which makes that particular fix unportable (see below).

### What sched_ext closes

| Gap | sched_ext | Why |
|---|---|---|
| 1 — Association | **not closed** | BPF must still be told; it can *hold* the association in task-local storage but provides no signal (INV-12). |
| 2 — Native `cpu.stat` | **not closed** | `update_curr_scx()` → `update_curr_common()` → the same `update_se()` (INV-9). BPF may keep private counters; `cpu.stat` is untouched. |
| 3 — Enforcement | **fully closed** | BPF owns dispatch for every task including helpers (INV-11); it never throttles a cfs_rq, it simply declines to dispatch. Charging and enforcement collapse into one policy (INV-10). |
| 4a — 1:N plumbing | **fully closed** | No `rq->donor` involvement at all under scx (INV-27); the association is a BPF map entry, so N-to-1 has no structural limit (INV-31). |
| 4b — quota semantics | **n/a** | Never a gap: absolute-microsecond budgets compose across CPUs under either scheduler. |
| 4c — weight semantics | **expressible, but unportably** | scx *permits* a BPF scheduler to replace per-runqueue weight competition with globally-ordered vtime DSQs — it supplies the mechanism, not the policy — in which charging helper runtime to the initiator delays it across every CPU (INV-32). CFS has no equivalent, so this fix cannot be carried back — see the scoping constraint below. |

Remaining after the prototype: Gaps 1 and 2 in full, plus total dependence on scx being loaded. Hence AC-4(a) stands independently and the prototype's deliverable is a measurement, not a mechanism (INV-23).

### Two candidate in-kernel approaches

- **Approach A — extend borrowed-context semantics.** Make the helper genuinely not-independently-runnable, executing the initiator's entity. Requires one `sched_entity` to run on N CPUs concurrently; the scheduler's model is one entity to one CPU. **Rejected as a "minimal" change.**
- **Approach B — cooperative enforcement.** Do not ask the scheduler to stop the helper; let the helper ask whether its charge context is over budget and bail. Viable here specifically because the consumer is cooperative kernel code with a natural bail-out point (INV-24), and because mm already enforces a cgroup limit this way (INV-22). **Recommended.**

### The resulting minimal ask — three pieces, mostly reuse

1. `cpu_charge_begin(cgroup)` / `cpu_charge_end()` on `current` — closes Gap 1. New, small.
2. Route `update_curr()`'s charging to that context while active — closes Gap 2 and performs the quota docking. Reuses existing machinery, subject to INV-26.
3. `cpu_charge_over_budget()` — building on the existing `cfs_rq_throttled()` predicate. Closes Gap 3 **by dissolving it**: no new *enforcement* mechanism, because the consumer stops itself.

**Neither piece 2 nor piece 3 is a thin wrapper, and the proposal must not present them as such.** Both face the same unresolved question from opposite ends: quota is charged at *every level* of the task's hierarchy (INV-26), so piece 2 must walk a hierarchy the helper is not part of — and piece 3 must then decide *which* `cfs_rq` to interrogate (the initiator's cgroup has one per CPU; the helper is on a different CPU) and under what locking, since CFS bandwidth state is manipulated under rq-lock assumptions on the current hierarchy. **The prototype's primary job is to settle both sides of this, plus the Gap 4b charging behaviour, before any API is proposed** — discovering the semantics are wrong is cheap in BPF and expensive in a patch series.

### Upstream sequencing

1. Land Tier 0 + Tier 1 + AC-4(a) — mergeable, CFS-only, dependent on none of the above.
2. Build the AC-4(c) prototype; produce the number: *a cgroup exceeds `cpu.max` by X% under stock CFS and respects it under the prototype.*
3. Use the prototype to settle Gap 4b's quota charging behaviour and INV-26's hierarchy-walk shape, **before** proposing an API.

> **Scoping constraint (important).** The demonstrator must confine its enforcement to **quota (`cpu.max`)** and must **not** demonstrate weight-based 1:N fairness. Quota composes across CPUs and is therefore what CFS could plausibly adopt, which is what the three-piece ask is about. A global-vtime weight fix (Gap 4c) would be genuinely impressive and genuinely **unportable** — it would demonstrate something CFS cannot adopt without a new fairness model, i.e. exactly the "large, open-ended scheduler-core change" AC-4(d)'s negative tests forbid asking for. Building it would spend effort producing evidence for a request that cannot be made.
4. Post to linux-mm and the scheduler list leading with the measurement, proposing the three-piece API, citing memcg as enforcement precedent (INV-22) and the prototype as evidence the semantics work.
5. If cooperative enforcement is rejected, the documented fallback is: keep AC-4(a)'s statistics, accept the enforcement gap, and rely on INV-8's self-limiting property — stated up front rather than discovered later.

## Feasibility Hints and Suggestions

> Reference only, not prescriptive.

### AC-1 (Tier 0): de-`__init`

Mechanical. Remove the six annotations in `kernel/padata.c` and two in `include/linux/padata.h` listed in AC-1, and drop the now-stale `__ref` comment block at `kernel/padata.c:92-99`. The substance of this task is the written pool-contention analysis, not the diff.

### AC-2 (Tier 1): padata-owned deprioritized workqueue

```c
/* In padata_init(), or lazily on first runtime multithreaded job --
 * ordering versus the earliest boot caller (mm_init.c:2143) must be
 * verified, since padata_do_multithreaded() runs during boot too. */
static struct workqueue_struct *padata_mt_wq;

static int padata_mt_wq_init(void)
{
        struct workqueue_attrs *attrs;
        int ret;

        padata_mt_wq = alloc_workqueue("padata_mt", WQ_UNBOUND, WQ_MAX_ACTIVE);
        if (!padata_mt_wq)
                return -ENOMEM;

        attrs = alloc_workqueue_attrs();
        if (!attrs) { /* destroy_workqueue + return -ENOMEM */ }

        /* nice 19 -> weight 15 vs 1024 at nice 0 (sched_prio_to_weight[]).
         * Helpers yield to essentially any normal-priority work, while the
         * calling thread keeps its own priority and its own share of the
         * job (see padata_do_multithreaded()'s inline helper call). */
        attrs->nice = 19;
        ret = apply_workqueue_attrs(padata_mt_wq, attrs);
        free_workqueue_attrs(attrs);
        return ret;
}
```

Then `kernel/padata.c:493` and `:495` queue to `padata_mt_wq` instead of `system_dfl_wq`. Whether boot-time callers keep the old workqueue is DEC-1.

### AC-4(a): cgroup `cpu.stat` charge-back sketch

> An earlier draft of this plan sketched folding the helper's delta into the initiator's `utime`/`stime`. That approach is **mechanically ineffective** — see INV-1 — and has been removed rather than left as a misleading starting point.

```c
/* Whole feature is inside #ifdef CONFIG_CGROUPS: __cgroup_account_cputime()
 * and __cgroup_account_cputime_field() are declared only under CONFIG_CGROUPS
 * (include/linux/cgroup.h), and the !CONFIG_CGROUPS branch stubs only the
 * cgroup_*() wrappers, not the __cgroup_*() forms. CONFIG_PADATA depends on
 * SMP alone, so PADATA=y/CGROUPS=n is a real config (INV-36). */

/* --- at job submission, in padata_do_multithreaded(): current is the
 *     initiator. Capturing inside the helper would yield the kworker's
 *     cgroup (root), which is the bug being fixed.
 *
 *     task->cgroups is RCU-protected and css_get() requires the caller to
 *     already hold a reference, so use the in-tree try-get retry pattern
 *     from cgroup_sk_alloc() (INV-34), not a bare cgroup_get(). */
/* No get_task_struct() needed: padata_do_multithreaded() is synchronous,
 * so the initiator is current and stays blocked in it for the whole job;
 * and every helper finishes touching ps before the completion that lets
 * this stack frame go away. The pointer is only ever compared, never
 * dereferenced. */
ps.initiator_task = current;      /* needed to tell caller from helper below */
rcu_read_lock();
while (true) {
        struct css_set *cset = task_css_set(current);

        if (likely(cgroup_tryget(cset->dfl_cgrp))) {
                ps.initiator_cgrp = cset->dfl_cgrp;
                break;
        }
        cpu_relax();
}
rcu_read_unlock();

/* --- in padata_mt_helper(), around the whole chunk loop.
 *
 *     CRITICAL: padata_mt_helper() runs on BOTH the helpers and the
 *     initiator itself, which calls it inline (kernel/padata.c:498-500).
 *     Instrumenting unconditionally would charge caller-participation a
 *     second time, on top of the native billing it already gets (INV-15).
 *     struct padata_work carries no caller/helper flag -- PADATA_WORK_ONSTACK
 *     is consumed by padata_work_init() and not stored (INV-37) -- so the
 *     distinction has to come from comparing against the stashed task. */
bool is_helper = (current != ps->initiator_task);
u64 before = is_helper ? task_sched_runtime(current) : 0;

/* ... while (job->size > 0) { claim chunk; job->thread_fn(...); } ... */

if (is_helper) {
        u64 delta = task_sched_runtime(current) - before;

        /* Mirror the wrapper's own guard: __cgroup_*() are the raw forms
         * and do NOT apply cgroup_account_cputime()'s cgroup_parent()
         * test. A root initiator has nothing useful to charge -- root's
         * cpu.stat is read from global kcpustat, not rstat -- so writing
         * there is a pointless dirtying of a counter nobody reads. */
        if (cgroup_parent(ps->initiator_cgrp)) {
                /* Total and split, as the scheduler does via two separate
                 * calls. Charging only the total lets cputime_adjust()
                 * smear this (pure kernel) time across user_usec/
                 * system_usec in the cgroup's existing ratio (INV-35). */
                __cgroup_account_cputime(ps->initiator_cgrp, delta);
                __cgroup_account_cputime_field(ps->initiator_cgrp,
                                               CPUTIME_SYSTEM, delta);
        }
}

/* MUST precede "++ps->nworks_fini" below: the last helper to increment
 * triggers complete(), after which the initiator wakes and drops its
 * reference -- a charge still in flight would touch a freed cgroup. */

/* --- after wait_for_completion() returns: */
cgroup_put(ps.initiator_cgrp);

/* No double-count against the helper's own cgroup: kworkers live in root,
 * and cgroup_account_cputime()'s cgroup_parent() guard means root-cgroup
 * tasks never enter the rstat tree at all (INV-29). That placement is
 * enforced rather than conventional -- workqueue workers carry
 * PF_NO_SETAFFINITY and cgroup migration refuses them (INV-33).
 *
 * Deliberately NOT done here:
 *  - touching the initiator's utime/stime or se.sum_exec_runtime
 *    (AC-4(b); INV-1, INV-4, INV-7);
 *  - docking the initiator cgroup's CFS-bandwidth runtime_remaining,
 *    which alone would throttle the initiator's threads without
 *    stopping the helper (AC-4(c)/(d); INV-5 as corrected). */
```

**Documented upgrade path, deliberately not the first implementation.** The measure-and-charge form above is correct *because* padata's helpers live in the root cgroup. A helper in a non-root cgroup would genuinely double-count — which is what the `WARN_ON_ONCE` exists to catch. The robust alternative redirects rather than adds: a `current->cpu_charge_cgrp` field plus two lines in `include/linux/cgroup.h`,

```c
cgrp = task->cpu_charge_cgrp ? : task_dfl_cgroup(task);
```

which is exact, needs no measurement at all, and holds regardless of the helper's cgroup. It is not proposed first because it adds a `task_struct` field, puts a load and branch in the scheduler's accounting hot path, carries a stale-redirect hazard across kworker reuse (a leaked redirect poisons every later work item on that worker), and modifies behaviour reached from `kernel/sched/` — at minimum a judgment call against DEC-3's gate. Not worth spending when the padata-local version is correct for how kworkers are actually placed.

**cgroup-v1 is not covered by either approach.** `cpuacct_charge(task, delta_exec)` (`kernel/sched/cpuacct.c:336-344`) sits in the same wrapper but takes a *task*, not a cgroup, so v1 `cpuacct` remains unredirected and will keep under-reporting. This is a real, permanent limitation of AC-4(a), not an oversight — it should be stated in the commit message rather than left for a reviewer to find.

## Dependencies and Sequence

### Milestones

1. **Tier 0**: de-`__init` padata; write the pool-contention analysis; build-test across `CONFIG_PADATA=y/n` and clang LTO.
2. **Tier 1**: add the padata-owned nice-19 workqueue; route multithreaded helper work to it; verify boot-caller ordering and behavior.
3. **Port mtcopy onto padata**: replace `copy_page_lists_mt()`'s internal `system_unbound_wq` fan-out and `flush_work()` join with a `padata_do_multithreaded()` call, preserving its external contract (AC-5). Depends on Milestones 1-2.
4. **Benchmark**: busy-system (throughput *and* caller latency) and idle-system (throughput). Depends on Milestone 3. Feeds the revisit triggers for both deferred TODOs.
5. **AC-4 prototype and writeup**: independent of Milestones 1-4; touches post-copy accounting, not dispatch.

## Task Breakdown

| Task ID | Description | Target AC | Tag (`coding`/`analyze`) | Depends On |
|---------|-------------|-----------|----------------------------|------------|
| task1 | Remove `__init`/`__initdata` from padata multithreaded paths and the stale `__ref` comment | AC-1 | coding | - |
| task2 | Write the `padata_works` pool-contention analysis for concurrent runtime callers | AC-1 | analyze | task1 |
| task3 | Build-test `CONFIG_PADATA=y`, `CONFIG_PADATA=n`, and clang LTO if available | AC-1 | analyze | task1 |
| task4 | Add padata-owned `WQ_UNBOUND` workqueue at nice 19; verify creation ordering vs. earliest boot caller | AC-2 | coding | task1 |
| task5 | Route multithreaded helper work (both `queue_work()` and `queue_work_node()` paths) to the new workqueue | AC-2 | coding | task4 |
| task6 | Decide DEC-1 (do boot callers keep `system_dfl_wq`?) and implement the chosen split, if any | AC-2 | coding | task5 |
| task7 | Port `copy_page_lists_mt()` to `padata_do_multithreaded()`, preserving its external contract | AC-5 | coding | task6 |
| task8 | Confirm AC-5: no leakage into `migrate_folios_batch_move()`/`MIGRATE_NO_COPY`/`static_call` dispatch; document RFC patch 9/9's fate | AC-5 | analyze | task7 |
| task9 | Busy-system benchmark: system throughput *and* caller-observed migration latency, vs. pool-disabled baseline | AC-2 | analyze | task7 |
| task10 | Idle-system benchmark: reproduce RFC v3's mtcopy speedup | AC-2 | analyze | task7 |
| task11 | Starved-helper test: confirm caller-participation completes the job under saturating pinned load | AC-3 | analyze | task7 |
| task12 | Evaluate task9's caller-latency result against TODO-2's revisit trigger; record the verdict either way | AC-2 | analyze | task9 |
| task13 | Evaluate task9/task10 against TODO-1's revisit trigger (is nice 19 sufficient, or is Tier 2 warranted?) | AC-2 | analyze | task9, task10 |
| task14 | Implement AC-4(a) under `CONFIG_CGROUPS` (INV-36): capture the initiator's cgroup at submission via the RCU + `cgroup_tryget()` retry pattern (INV-34), released with `cgroup_put()`; stash `initiator_task` and skip instrumentation when `current` is the inline caller (INV-37, INV-15); measure per-helper-per-job (INV-30); charge **both** `__cgroup_account_cputime()` and `__cgroup_account_cputime_field(CPUTIME_SYSTEM)` (INV-35) under an explicit `cgroup_parent()` guard, before incrementing `nworks_fini` | AC-4 | coding | task7 |
| task15 | Measure AC-4(a): cgroup `cpu.stat` `usage_usec` for a migration-heavy workload, pool enabled vs. disabled | AC-4 | analyze | task14 |
| task16 | Instrument and measure the caller-drained-chunk fraction across load levels, to support or refute INV-8's self-limiting-misattribution argument | AC-4 | analyze | task7 |
| task17a | AC-4(c.1): prototype-only padata association tracepoint `(current, initiator)` + BPF task-local storage recording helper→initiator charge-back | AC-4 | coding | task7 |
| task17b | AC-4(c.2): scx `cgroup_set_bandwidth` + refuse to dispatch helpers once the initiator's budget is spent | AC-4 | coding | task17a |
| task18 | AC-4(c.3): produce the gap measurement (cgroup `cpu.max` overrun under stock CFS vs. respected under the prototype) and settle the two questions the in-kernel ask depends on: the hierarchy-walk shape for redirected charging (INV-26) and the **quota-only** 1:N charging behaviour (Gap 4b). Weight-based 1:N (Gap 4c) is out of scope by construction (INV-32) | AC-4 | analyze | task17b |
| task19 | **GATED (TODO-4, DEC-3)** — AC-4(d): the three-piece minimal-primitive proposal per "The Enforcement Gap". Not started until task18 is complete *and* the demonstrator is accepted upstream | AC-4 | analyze | task18 + upstream acceptance |
| task20 | Record AC-4(b)'s explicit non-goal and its justification (INV-1, INV-4, INV-7) in the commit message and plan text | AC-4 | analyze | - |
| task21 | **GATED (TODO-4, DEC-3)** — AC-4(c.4): `scx_bpf_cgroup_account_cputime()`-style kfunc so `cpu.stat` reflects helper time under scx. Requires a kernel change (INV-27, INV-28); not a precondition for task18 | AC-4 | coding | task18 + upstream acceptance |

## Pending User Decisions

- DEC-1: Should boot-time padata callers (`mm/mm_init.c`, `mm/hugetlb.c`) keep queueing to `system_dfl_wq` at normal priority, or also move to the nice-19 workqueue?
  - Claude Position: Keep boot callers on `system_dfl_wq`. Their entire purpose is finishing boot faster on an otherwise-idle machine; deprioritizing them has no upside and risks a measurable boot-time regression in exactly the workload padata multithreading was originally written for (`004ed42638f44`). This implies a small `struct padata_mt_job` extension (a flag, or a `nice` field) so callers select their priority, rather than padata switching globally.
  - Tradeoff Summary: A per-job selector is a slightly larger interface change but avoids touching well-established boot behavior. A global switch is a smaller diff but changes existing callers for no benefit.
  - Decision Status: PENDING — cheap either way, but worth deciding before task4/task5 so the workqueue-creation point and the job interface are settled together.
- DEC-2: How much effort to invest in AC-4(c)'s sched_ext demonstrator, given it is throwaway by construction?
  - Claude Position: Enough to produce one credible number and no more. The deliverable is "a cgroup exceeds `cpu.max` by X% under stock CFS and respects it under the prototype," which is what converts a request for advice into a reported bug with a demonstrated fix. Prototype generality, tuning, and additional scx ops are pure cost — the scheduler is scaffolding around a measurement. The associated risk is the opposite of under-investment: a polished prototype invites "why not just use sched_ext then," which is precisely the conclusion AC-4(d) exists to avoid, since it would leave stock-CFS systems (the ones Byungchul's objection concerns) unfixed.
  - Tradeoff Summary: Under-investing yields a number nobody believes. Over-investing yields a solution nobody can merge and an argument that undercuts itself. The narrow target is a reproducible measurement with a prototype simple enough that its correctness is self-evident.
  - Decision Status: PENDING — confirm before task17 begins, since it sets the prototype's scope.

- DEC-3: **RESOLVED** — scope and ordering of scheduler-side work.
  - Decision (repository owner): (i) all `kernel/sched/` changes are deferred to TODO-4 and gated on the AC-4(c) sched_ext demonstrator being complete **and** accepted upstream; (ii) within AC-4(c), `cpu.stat` work is ordered last (c.4), behind association, enforcement, and the gap measurement; (iii) AC-4(a) — the CFS-side, padata/mm-side `__cgroup_account_cputime()` charge-back — is **not** covered by (ii) and stays early, alongside Tier 0/Tier 1.
  - Rationale: (d) is the only part touching `kernel/sched/`, the hardest to get accepted, and derives all its persuasive weight from (c)'s measurement — posting it earlier spends credibility (c) has not yet earned. Within (c), `cpu.stat` is the one sub-part that cannot be done in BPF at all (INV-27, INV-28) and would itself need a kernel change, so it is both last and gated. AC-4(a) is exempt because it is scheduler-agnostic, needs no kfunc, is independently mergeable now, and makes a real cost visible before anything enforces it.
  - Decision Status: RESOLVED.

## Implementation Notes

### Code Style Requirements

- Follow existing `kernel/padata.c` and `drivers/migoffcopy/` conventions.
- Code comments describe only current structure — no references to this plan, the RFC thread, or "AC-N"/"Tier N" labels in actual code comments.
- Commit messages credit the originating RFC thread and its authors (Shivank Garg, Zi Yan, Mike Day) where this work builds on their patches, and should Cc Daniel Jordan (padata multithreaded author) and Tejun Heo (workqueue) on the padata changes.

## Relevant References

- `20250923174752.35701-1-shivankg@amd.com` — RFC v3 0/9 cover letter (motivation, performance data, open questions)
- `20250923174752.35701-7-shivankg@amd.com` — RFC v3 6/9, `mtcopy: introduce multi-threaded page copy routine` (the driver this plan re-hosts on padata)
- `633F4EFC-13A9-40DF-A27D-DBBDD0AF44F3@nvidia.com` — Zi Yan's reply: the padata experiment, the `kernel/padata.c` hacking requirement, the CPU-time-attribution concern, and RFC patch 9/9 no longer applying
- `20251020082800.GA28427@system.software.com`, `20251112021217.GA45963@system.software.com` — Byungchul Park's busy-system regression objection and follow-up
- `20251002121009.00005899@huawei.com` — Jonathan Cameron's review of the pluggable-migrator framework (AC-5's "don't touch this" boundary)
- `004ed42638f44` — Daniel Jordan, "padata: add basic support for multithreaded jobs"; establishes that `__init` was a day-one scoping choice, and that Zi Yan and Tejun Heo were both Cc'd
- `kernel/padata.c:404-507` — `padata_mt_helper()` and `padata_do_multithreaded()`; caller-participation at :498-500, the completion condition at :429-434 and :503
- `kernel/workqueue.c:2874` — where a pool's `nice` is applied to its workers; the Tier 1 hook and the Tier 2 touch point
- `kernel/sched/core.c:7572-7586`, `:1524-1536`, `:10606-10615`; `kernel/sched/sched.h:2511` — `fair_sched_class` mapping for both nice and `SCHED_IDLE`, the weight table, and `WEIGHT_IDLEPRIO`

## Investigation Results

Findings verified by reading this tree, recorded so the plan's claims can be re-checked without re-deriving them. Group A backs AC-4; Group B backs AC-1 through AC-3 and the Deferred Work items. Line numbers are as of the tree this plan was written against and should be treated as anchors, not guarantees.

### Group A — CPU-time attribution (AC-4)

- **INV-1: `utime`/`stime` are a ratio, not a total — bumping them is a no-op.** `cputime_adjust()` (`kernel/sched/cputime.c:740-807`) takes `rtime = curr->sum_exec_runtime` as authoritative, then rescales: `stime = mul_u64_u64_div_u64(stime, rtime, stime + utime); utime = rtime - stime;`. Adding to `p->stime` therefore only shifts the user/system split of an unchanged total. Reached from `task_cputime_adjusted()` (`:811-820`), i.e. the path behind `/proc/<pid>/stat` and `getrusage()` in the default `!CONFIG_VIRT_CPU_ACCOUNTING_NATIVE` config. **Consequence: the only lever for per-task visibility is `se.sum_exec_runtime` itself — see INV-7 for why that is undesirable.** This finding invalidated an earlier draft of this plan's AC-4 sketch.
- **INV-2: the kernel already has a runner/donor split, and cgroup time follows the donor.** `update_se()` (`kernel/sched/fair.c:1355-1394`) sends `sum_exec_runtime`, `account_group_exec_runtime()` and `account_mm_sched()` to `running = rq->curr`, but calls `cgroup_account_cputime(donor, delta_exec)` under the in-tree comment *"cgroup time is always accounted against the donor"*. This is proxy-exec's mechanism and is the precedent AC-4(a) follows.
- **INV-3: the cgroup charge hook takes a cgroup, not a task.** `cgroup_account_cputime()` (`include/linux/cgroup.h:821-831`) is a thin wrapper that derives `task_dfl_cgroup(task)` and calls `__cgroup_account_cputime(cgrp, delta_exec)` (declared `:817`, defined `kernel/cgroup/rstat.c:630`). The inner function is directly usable with an arbitrary cgroup, which is what makes AC-4(a) a small patch rather than surgery.
- **INV-4: even proxy-exec does not redirect per-task runtime.** Per INV-2, `running->se.sum_exec_runtime += delta_exec` and `account_group_exec_runtime(running, ...)` go to the physically-running task even when a donor exists. The kernel's own line is therefore drawn precisely at *cgroup-level attribution is redirectable, per-task attribution is not* — there is no in-tree precedent for what AC-4(b) declines to do.
- **INV-5: quota enforcement is cfs_rq-local, so charging *alone* is insufficient — but not, as an earlier revision of this plan claimed, wrong.** `account_cfs_rq_runtime()` (`kernel/sched/fair.c:6536-6560`, called from `update_curr()` at `:2022`) docks `cfs_rq->runtime_remaining` on the cfs_rq the running entity is enqueued on and, on exhaustion, calls `throttle_cfs_rq()` to dequeue *that* cfs_rq. **Correction:** this plan previously concluded that docking the initiator's cgroup while the helper lives in the root cgroup is "strictly worse than not charging," because it throttles the initiator's other threads while the helper runs on. That holds only for charging *in isolation*. Paired with cooperative back-off (INV-22, INV-24) the pair is correct: the initiator's tasks throttle because the cgroup genuinely did consume that CPU, and the helper stops too because it polls and bails. The real obstacle is therefore narrower than stated — it is the *absence of any stop signal reaching the helper*, not the charging itself. This correction materially shrinks the minimal in-kernel ask; see "The Enforcement Gap" section.
- **INV-6: proxy-exec is 1:1; this problem is 1:N with the initiator also running.** Proxy-exec transfers one blocked donor's scheduling context to one runner. Here one initiator has N concurrent helpers plus its own caller-participation share (INV-15). A cgroup entity carries a single weight, so N concurrent charges are coherent for additive statistics but not for weight-proportional scheduling. Proxy-exec's machinery therefore cannot be reused directly even setting aside its blocked-on-mutex trigger.
- **INV-7: `se.sum_exec_runtime` has readers well beyond `/proc`.** POSIX CPU timers and `RLIMIT_CPU` via `read_sum_exec_runtime()`/`thread_group_cputime()` (`kernel/sched/cputime.c:306-318`, `:350`); NUMA-balancing scan-period heuristics (`kernel/sched/fair.c:3531`, `:4081`, `:4327-4328`, `:4400`); `RLIMIT_RTTIME` (`kernel/sched/rt.c:2524`); `/proc/<pid>/sched` avg_atom/avg_per_cpu (`kernel/sched/debug.c:1377`, `:1383`). Inflating it could fire `SIGXCPU` on account of migration work (arguably correct, but a user-visible behavior change) and would perturb NUMA scan periods — ironic, given NUMA balancing is one of migration's main callers.
- **INV-8: misattribution is self-limiting.** padata steals chunks dynamically (INV-15), so the initiating thread's share is not `1/(N+1)` but however much it drains. Under load, nice-19 helpers get little CPU and the initiator does most of the copy — charged correctly, because it really did run it. On an idle system helpers do most of it and attribution is mostly wrong, but there is no contention for that to distort. **Attribution error is largest exactly when it matters least.** This is the strongest available argument that deferring enforcement is defensible; task16 measures it rather than assuming it.
- **INV-9: sched_ext does not change cgroup CPU accounting.** `update_curr_scx()` (`kernel/sched/ext/ext.c:1333-1349`) calls `update_curr_common()` (`kernel/sched/fair.c:1977-1980`), which is `update_se(rq, &rq->donor->se)` — the *same* function as INV-2. So scx tasks flow through identical `cgroup_account_cputime(donor, ...)` accounting. **sched_ext offers no lever for the statistics half; AC-4(a) is unchanged and still required under scx.**
- **INV-10: sched_ext delegates cgroup bandwidth enforcement entirely to the BPF scheduler.** `struct sched_ext_ops::cgroup_set_bandwidth` (`kernel/sched/ext/internal.h:684-703`) receives `cpu.max`'s `period_us`/`quota_us`/`burst_us` with the explicit kernel-doc statement that *"The specific control mechanism and thus the interpretation of @period_us and burstiness is up to the BPF scheduler."* Confirmed by absence: `kernel/sched/ext/ext.c` contains no `account_cfs_rq_runtime`, `runtime_remaining`, or throttling logic. **This dissolves INV-5's obstacle in principle** — a BPF scheduler controls dispatch for both the initiator and the helpers, so it can charge the initiator's budget *and* stop dispatching helpers, unifying accounting with enforcement under one policy. Requires `CONFIG_EXT_GROUP_SCHED` (`init/Kconfig:1199`).
- **INV-11: sched_ext covers kernel threads by default.** `SCX_OPS_SWITCH_PARTIAL` (`kernel/sched/ext/internal.h:128-133`) is opt-in; without it, all `SCHED_NORMAL` tasks — including kworkers — are handled by the BPF scheduler. Padata helpers would therefore be under BPF control without special arrangement. `scx_bpf_task_cgroup()` (`kernel/sched/ext/ext.c:10425-10435`) lets BPF read a task's cgroup.
- **INV-12: sched_ext lacks a kernel→BPF channel for the helper↔initiator association.** No in-tree concept expresses "this kworker is currently executing work on behalf of task X." A BPF scheduler cannot infer it. Closing this needs an explicit signal from padata — a tracepoint around `padata_mt_helper()` carrying `(current, initiator)` that BPF attaches to and records in task-local storage is the smallest such mechanism, but it is new observable surface.
- **INV-13: sched_ext is already on the RFC's own roadmap.** The RFC v3 cover letter's FUTURE WORK item 2 reads *"Enhance multi-threaded CPU copying for platform-specific scheduling of worker threads to optimize bandwidth utilization. Explore sched-ext for this,"* citing the LSFMM discussion. Andrea Righi's suggestion converges with the series' own stated direction.
- **INV-22: cgroup limits are already enforced cooperatively elsewhere in mm.** memcg's `memory.high` does not rely on the scheduler forcibly stopping an over-limit task. `__mem_cgroup_handle_over_high()` (`mm/memcontrol.c:2550-2600`) accumulates `current->memcg_nr_pages_over_high`, computes `penalty_jiffies`, and applies the delay at a safe point — the code's own comment describes it as *"return path where reclaim is always able to block."* This is direct in-tree precedent for the enforcement shape AC-4(d) proposes: the consumer voluntarily throttles itself at a known-safe boundary, rather than the scheduler dequeuing it from a hierarchy it does not belong to. It is also an *mm* precedent, which matters when the proposal is addressed jointly to mm and scheduler maintainers.
- **INV-23: sched_ext's role is evidence, not deployment.** Recorded to prevent the reversal in TODO-3 from being re-litigated: INV-9 through INV-13 were originally assembled to evaluate sched_ext as a shipping mechanism, under which framing its limitations are disqualifying. Under the demonstrator framing (AC-4(c)) only INV-9 still constrains anything — the statistics half is untouched by scx, so AC-4(a) must stand on its own. INV-10's delegation of bandwidth enforcement to BPF is what makes the demonstration possible at all; INV-12's missing association channel becomes prototype scaffolding rather than proposed ABI.
- **INV-24: a padata helper can safely bail mid-job.** `padata_mt_helper()` (`kernel/padata.c:404-434`) claims one chunk per iteration under `ps->lock`, releases the lock across `job->thread_fn()`, and on loop exit increments `nworks_fini` and completes if last. A helper that breaks out early therefore leaves unclaimed chunks for others and still satisfies the completion condition. This is safe because the caller runs the same helper inline *before* waiting (INV-15) and its own loop exits only when `job->size` reaches 0 — so all chunks are always claimed regardless of helper behavior. **Consequence: cooperative back-off requires no new completion machinery**, which is what makes AC-4(d)'s minimal-primitive proposal cheap for this caller. Note this is independent of TODO-2, which concerns waiting for helpers that never *started*.

- **INV-25: proxy-exec's enforcement is emergent from borrowed-context exclusivity, which is exactly what mtcopy lacks.** `rq->donor` is set to whatever `pick_next_task()` returned (`kernel/sched/core.c:7148-7156`: `next = pick_next_task(rq, &rf); ... rq_set_donor(rq, next); if (unlikely(next->is_blocked)) next = find_proxy_task(rq, next, &rf);`). So the donor is the *scheduler-selected* task and the runner is a substitute executing on the donor's context. Because `cfs_rq->curr` tracks the donor (`kernel/sched/fair.c:1985-1996`), `account_cfs_rq_runtime()` already docks the **donor's** cgroup quota, and `throttle_cfs_rq()` dequeues the **donor's** cfs_rq — after which the donor is no longer picked and the runner, having no independent right to run, stops for free. **The kernel therefore already implements correct cross-hierarchy charging and enforcement — but only when the consumer runs solely on borrowed context.** A padata helper is an independently-schedulable kworker with its own `sched_entity`, `cfs_rq` and weight, so none of this transfers. This is the precise statement of Gap 3.
- **INV-26: quota is charged across the whole hierarchy, so redirecting it is not a one-line change.** `task_tick_fair()` (`kernel/sched/fair.c:14851-14859`) walks `for_each_sched_entity(se)` from the task's own entity up through every enclosing task_group, calling `entity_tick()` → `update_curr()` → `account_cfs_rq_runtime()` at each level. Redirecting charge to a different cgroup means walking *that* task_group's hierarchy on this CPU (`tg->cfs_rq[cpu]`, `tg->parent`) — a hierarchy the helper's own `se` is not part of. Tractable, but genuinely new code rather than passing a different `cfs_rq` to one call. **This is the single most important open question for the AC-4(c) prototype to pin down**, because it is where "minimal in-kernel change" either holds or breaks.

- **INV-27: under sched_ext the donor/runner split is compiled out entirely — `donor` and `curr` are the same memory.** `struct rq` (`kernel/sched/sched.h:1151-1160`) declares them as two fields only under `CONFIG_SCHED_PROXY_EXEC`; otherwise they are a `union`, i.e. one word with two names, and `rq_set_donor()` is a no-op stub (`:1449-1452`). Because `SCHED_PROXY_EXEC depends on !SCHED_CLASS_EXT` (`init/Kconfig:936-943`), **any kernel with sched_ext has `donor == curr` literally**, so `cgroup_account_cputime(donor, ...)` in `update_se()` *is* `cgroup_account_cputime(curr, ...)`. This is strictly stronger than INV-9's "same code path": there is no separate donor to redirect to. It also means proxy execution and sched_ext **cannot coexist in one build**, so the AC-4(c) demonstrator and any proxy-exec experiment are two separate kernels.
- **INV-28: no scx kfunc writes cgroup CPU accounting.** The full `BTF_ID_FLAGS(func, scx_bpf_*)` set in `kernel/sched/ext/ext.c` covers DSQs, vtime, slices, dispatch, idle masks, CPU/node queries, `cpuperf`, and diagnostics — nothing that charges cputime. Combined with INV-27, a BPF scheduler can therefore do delegated billing **for its own dispatch decisions** (real enforcement, since it owns dispatch for every task including helpers — INV-10, INV-11) but **cannot** feed the kernel's native `cpu.stat`/rstat. Making it do so would require a new `scx_bpf_cgroup_account_cputime()`-style kfunc built on `__cgroup_account_cputime()` (INV-3) — a kernel change, and one that would only ever help scx users, so it is no substitute for AC-4(a). This is why AC-4(c.4) is ordered last and shares TODO-4's gating.

- **INV-29: root-cgroup tasks never enter the rstat tree, which is why AC-4(a) does not double-count.** `cgroup_account_cputime()` (`include/linux/cgroup.h:821-831`) guards its charge with `if (cgroup_parent(cgrp))` — the root cgroup has no parent, so a kworker living in root contributes *nothing* to rstat. On the read side, root's `cpu.stat` is not computed from rstat either: `cgroup_base_stat_cputime_show()` branches on the same `cgroup_parent(cgrp)` test and falls back to `root_cgroup_cputime(&bstat)` from global kcpustat (`kernel/cgroup/rstat.c:729-736`). Explicitly charging the initiator's cgroup therefore fills a real hole rather than duplicating an existing charge, and rstat propagation up the initiator's ancestors is correct. **The argument depends on padata's helpers actually being in root** — hence AC-4(a)'s `WARN_ON_ONCE`, and hence the `cpu_charge_cgrp` redirect being kept as the documented upgrade path for the day that stops holding. Note the write side itself is trivial: `__cgroup_account_cputime()` (`kernel/cgroup/rstat.c:629-637`) is a `+=` on a per-CPU counter plus a dirty mark, with hierarchy aggregation deferred to flush-on-read.
- **INV-30: `task_sched_runtime()` on a running task takes the rq lock.** Its lockless 64-bit fast path is guarded by `if (!p->on_cpu || !task_on_rq_queued(p))` (`kernel/sched/core.c:5674-5700`); for `current` both are true, so it falls through to `task_rq_lock()` and may call `update_curr()`. Fine twice per helper per job; not fine per chunk inside a copy loop. This fixes AC-4(a)'s measurement granularity at per-helper-per-job.

- **INV-31: sched_ext has no structural 1:N limit, because it does not use `rq->donor` at all.** With `CONFIG_SCHED_CLASS_EXT` the donor/runner split is compiled out entirely (INV-27), so none of proxy execution's constraints apply: no single per-rq donor field, no linear `blocked_on` chain, no forced migration onto one runqueue (INV-6, INV-25). A BPF scheduler holds the helper→initiator association in task-local storage or a map, where "N helpers point at one initiator" is simply N entries. **Gap 4a is therefore fully closed under scx and remains open under CFS.**
- **INV-32: scx can express 1:N *weight* fairness because it replaces per-runqueue weight competition with globally-ordered vtime.** Vtime-ordered dispatch queues are first-class in sched_ext: `p->scx.dsq_vtime`, the `scx_dsq_priq_less()` comparator (`kernel/sched/ext/ext.c:1352-1360`), and the `scx_bpf_dsq_insert_vtime` / `scx_bpf_dsq_move_set_vtime` kfuncs; DSQs may be shared globally across CPUs. Charging helper runtime into the initiator's vtime therefore delays the initiator on *every* CPU, which is a working answer to Gap 4c. **CFS has no equivalent** — its weight is a per-`cfs_rq` ratio with no cross-CPU composition — so this answer is not portable back. **Consequence, and the reason it matters strategically: the part sched_ext fixes best is the part that cannot be asked for.** Hence the demonstrator is scoped to quota only; see "The Enforcement Gap" section's scoping constraint.

- **INV-33: padata's kworkers cannot leave the root cgroup — the placement AC-4(a) depends on is enforced, not conventional.** Workqueue workers are created with `kthread_bind_mask()` (`kernel/workqueue.c:2875`), which sets `PF_NO_SETAFFINITY` (`kernel/kthread.c:577`), and cgroup migration explicitly refuses such tasks: `if (tsk->no_cgroup_migration || (tsk->flags & PF_NO_SETAFFINITY)) { tsk = ERR_PTR(-EINVAL); ... }` (`kernel/cgroup/cgroup.c:3068-3077`), under a comment about kthreads becoming trapped in cpusets. **This upgrades INV-29's no-double-count argument from "true in practice" to "guaranteed by the cgroup code"**, and correspondingly downgrades AC-4(a)'s root-cgroup check from a tripwire against a plausible future to an assertion of an enforced invariant.
- **INV-34: `cgroup_get()` on a freshly-read `task_dfl_cgroup()` is not a valid reference acquisition.** `css_get()`'s kernel-doc states plainly *"The caller must already have a reference"* (`include/linux/cgroup_refcnt.h:2-13`), and `task->cgroups` is RCU-protected. The in-tree pattern for stashing `current`'s default cgroup beyond the current RCU section is `rcu_read_lock()` around a retry loop calling `cgroup_tryget(cset->dfl_cgrp)` — see `cgroup_sk_alloc()` (`kernel/cgroup/cgroup.c:7397-7414`), which does exactly this, including a separate branch for interrupt context. An earlier revision of AC-4(a)'s sketch used the bare `cgroup_get()` form and was wrong.
- **INV-35: charging only `__cgroup_account_cputime()` gets `usage_usec` right and the user/system split wrong.** `cpu.stat` reports `usage_usec`, `user_usec` and `system_usec`; the latter two are not stored as reported but *derived* on read, by `cputime_adjust(&cgrp->bstat.cputime, &cgrp->prev_cputime, ...)` (`kernel/cgroup/rstat.c:729-736`) — the same ratio-rescaling as INV-1, applied per cgroup. So bumping only `sum_exec_runtime` does not leave the split untouched: it scales both fields up in the cgroup's **pre-existing** user:system ratio, meaning time that was 100% kernel can be reported as mostly user time. The total stays correct; the attribution within it does not. The fix is to mirror the scheduler and call both `__cgroup_account_cputime()` and `__cgroup_account_cputime_field(cgrp, CPUTIME_SYSTEM, delta)` (`kernel/cgroup/rstat.c:629-658`) — different fields, no double-count.

- **INV-36: `PADATA=y, CGROUPS=n` is a real configuration, so AC-4(a) must be `CONFIG_CGROUPS`-guarded.** `__cgroup_account_cputime()` and `__cgroup_account_cputime_field()` are declared only inside `#ifdef CONFIG_CGROUPS` (`include/linux/cgroup.h:797-854`); the `#else` branch stubs only the `cgroup_account_cputime()`/`cgroup_account_cputime_field()` *wrappers*, not the raw `__cgroup_*()` forms. `CONFIG_PADATA` depends on `SMP` alone (`init/Kconfig:2291-2293`), so unguarded direct calls break that build. Related: because AC-4(a) calls the raw forms, it also bypasses the wrapper's `if (cgroup_parent(cgrp))` test and must reproduce it explicitly — otherwise a root-cgroup initiator gets a pointless rstat write into a counter nobody reads (root's `cpu.stat` comes from global kcpustat, INV-29).
- **INV-37: `padata_mt_helper()` runs on the initiator too, and `struct padata_work` carries no caller/helper flag.** `padata_do_multithreaded()` invokes the helper inline on the calling thread (`kernel/padata.c:498-500`, INV-15), so instrumentation placed naively inside `padata_mt_helper()` would charge caller-participation a second time on top of the native billing it already receives — silently violating AC-4(a)'s own no-double-count rule. `PADATA_WORK_ONSTACK` cannot be used to tell them apart: it is consumed by `padata_work_init()` to choose `INIT_WORK_ONSTACK()` vs `INIT_WORK()` and is never stored in `struct padata_work`, whose only members are `pw_work`, `pw_list` and `pw_data` (`kernel/padata.c:26-32`, `:99-106`). The distinction must therefore be made by stashing the initiator's `task_struct *` in the job state and comparing `current` against it. That pointer needs no `get_task_struct()`, and the reason should be stated so a reviewer does not have to ask: `padata_do_multithreaded()` is synchronous, so the initiator *is* `current` and remains blocked inside it for the job's duration; every helper finishes touching `ps` before the completion that permits the stack frame to go away; and the pointer is only ever compared, never dereferenced.

### Group B — deprioritization and padata (AC-1 through AC-3, Deferred Work)

- **INV-14: padata's `__init` was a day-one scoping choice.** `git show 004ed42638f44` (Daniel Jordan, "padata: add basic support for multithreaded jobs", 2020) shows `__init` present on `padata_do_multithreaded()`, `padata_mt_helper()`, `padata_work_alloc_mt()` and `padata_works_free()` in the original submission, when the only callers were boot-time. Not a correctness barrier. Zi Yan and Tejun Heo were both Cc'd on that commit.
- **INV-15: padata already runs the job on the calling thread.** `padata_do_multithreaded()` (`kernel/padata.c:498-500`) calls `padata_mt_helper()` inline on the caller under the comment *"Use the current thread, which saves starting a workqueue worker,"* and that helper's `while (job->size > 0)` loop (`:411-427`) drains chunks until the job is empty. A fully-starved helper pool therefore degrades to today's single-threaded behavior by construction. This is why AC-3 requires no bespoke fallback, and it underpins INV-8.
- **INV-16: the completion condition waits for every helper, including ones that never ran.** `ps.nworks` counts the caller plus every allocated work item — `padata_work_alloc_mt()` (`kernel/padata.c:109-125`) starts its counter at 1 *"because the current task participates in the job"* and returns it — and the completion fires only at `nworks_fini == nworks` (`:429-434`). So `wait_for_completion()` (`:503`) holds the caller in `TASK_UNINTERRUPTIBLE` until each queued helper is scheduled at least once, even after the caller has drained the entire job. Present at Tier 1, not only Tier 2; harmless in today's boot-only use. Backs TODO-2.
- **INV-17: both nice 19 and `SCHED_IDLE` are `fair_sched_class`.** `__setscheduler_class()` (`kernel/sched/core.c:7572-7586`) returns `&fair_sched_class` for everything that is not dl/rt/scx; `set_load_weight()` (`:1524-1536`) gives `task_has_idle_policy()` tasks `WEIGHT_IDLEPRIO`. Both are proportional-share, so helpers are slowed but never indefinitely starved — this bounds INV-16's worst case and is why TODO-2 is expected to be tolerable at Tier 1.
- **INV-18: weight arithmetic.** `sched_prio_to_weight[]` (`kernel/sched/core.c:10606-10615`): nice 0 = 1024, nice 19 = 15. `WEIGHT_IDLEPRIO` = 3 (`kernel/sched/sched.h:2511`). Nice 19 buys ~68x deprioritization; `SCHED_IDLE` ~341x, i.e. only ~5x beyond nice 19. This ratio is why Tier 1 is expected to be sufficient and Tier 2 is deferred pending measurement.
- **INV-19: `struct workqueue_attrs` carries `nice` but no scheduling policy.** `include/linux/workqueue.h:148-152`; applied to workers at `kernel/workqueue.c:2874` via `set_user_nice(worker->task, pool->attrs->nice)`. That single site is both Tier 1's existing hook (through `apply_workqueue_attrs()`) and Tier 2's touch point. Padata currently queues to the shared `system_dfl_wq` (`kernel/padata.c:493`, `:495`), an unbound workqueue it does not own (`kernel/workqueue.c:8061`).
- **INV-20: setting a kernel worker's scheduling policy is an established pattern.** `sched_setattr_nocheck()` is `EXPORT_SYMBOL_GPL` (`kernel/sched/syscalls.c:769-773`), used on a `kthread_worker`'s task in `drivers/cpufreq/cppc_cpufreq.c:240` and on a kthread in `kernel/sched/cpufreq_schedutil.c:691`. `SCHED_IDLE` passes `valid_policy()` (`kernel/sched/sched.h:218-222`). Tier 2's blocker is workqueue plumbing, not scheduler API availability.
- **INV-21: the `padata_works` pool is fixed-size and degrades gracefully.** `num_possible_cpus()` entries allocated once in `padata_init()` (`kernel/padata.c:1102-1107`), guarded by one spinlock (`:34-36`). `padata_work_alloc()` returns `NULL` when exhausted (`:83-84`) and `padata_work_alloc_mt()` simply stops allocating (`:113-121`), so concurrent runtime callers reduce each other's helper count rather than failing. Backs AC-1's required analysis.
