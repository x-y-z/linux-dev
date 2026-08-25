# Restructure migrate_pages() Into Per-Engine Dispatch With Deduplicated Batch/Sync Retry Logic

## Goal Description

`migrate_pages()` and its call graph (`mm/migrate.c`) currently mix three structurally distinct migration mechanisms — hugetlb folios, non-LRU `movable_ops` pages (balloon/zsmalloc/offline), and regular (possibly large/THP) folios — with only hugetlb cleanly isolated into its own function (`migrate_hugetlbs()`). `movable_ops` pages are threaded through the regular-folio code path via four scattered `page_has_movable_ops()` branches. On top of that, the regular-folio "engine" itself (`migrate_pages_batch()` + `migrate_pages_sync()`) duplicates real bookkeeping logic (a shadow `astats` struct manually merged with asymmetric, easy-to-miss rules) and expresses its async-then-sync-fallback retry policy as a `while` loop shaped differently from every other retry loop in the file.

This refactor restructures the whole call graph — rewritten from first principles around one goal: **each distinct migration mechanism gets its own function, and all retry logic across the file follows the same shape** (a bounded `for (pass...)` loop owning its own retry state directly, matching the existing, already-correct `migrate_hugetlbs()` pattern). It is overwhelmingly a **structural/readability refactor with no functional change intended** — no batching, DMA offload, or hardware-acceleration logic is implemented, and no Kconfig/drivers/DMA-dependent code is touched — with one accepted, narrow, and explicitly documented family of exceptions, all confined to the folio sync fallback's retry path: adopting the uniform pass-major retry-loop shape introduces `-ENOMEM`-triggered retry-ordering divergences from today's `migrate_pages_sync()`, plus a retry-economy divergence triggered by any move-step `-EAGAIN` (repeating allocation/lock/unmap work today's in-place retry avoids) (see AC-7's "Accepted retry-ordering and retry-economy exception" and AC-7.1(g)/(h)); outside of a `-ENOMEM` or a move-step `-EAGAIN` occurring during the sync fallback, substantive outcomes and counts are unchanged (a cosmetic, count-neutral returned-folio list-ordering effect can additionally arise from any `-EAGAIN` interleaving — see AC-7). Its purpose is to leave migration cleanly dispatched by mechanism before any future batch-copy/offload work needs to reason about which folios are even eligible for batching (external RFC discussions of "batch copying and hardware offload" are cited only as evidence of what shape a future seam should accommodate, not as content to be ported or applied).

This design was arrived at through direct, iterative discussion with the repository owner (Zi Yan) after an initial narrower draft (extract `migrate_folios_unmap()` only) had already converged through 3 rounds of Codex review. That narrower piece is **not discarded** — it's Part 1 / Milestone 1 below, unchanged, and still the right first step. Parts 2-4 are new, designed in this conversation, and have since been through 10 rounds of independent Codex review — see Claude-Codex Deliberation for the full history; both Part 1 and Parts 2-4 are now judged `converged`.

### The four parts

1. **Extract `migrate_folios_unmap()`** from `migrate_pages_batch()`'s inline unmap loop (mm/migrate.c:1811-2021), mirroring the existing `migrate_folios_move()`/`migrate_folios_undo()` split in the same file. *(Unchanged from the original narrower plan — already Codex-converged.)*
2. **Isolate `movable_ops` pages into their own `migrate_movable_ops_pages()` pass**, structured exactly like `migrate_hugetlbs()` (bounded `for (pass...)` loop, own retry state, single-attempt per-folio primitive), called from `migrate_pages()` before the `NR_MAX_BATCHED_MIGRATION` batch-splitting loop. This removes three of the four `page_has_movable_ops()` branches — from `migrate_folio_unmap()`, `migrate_folio_move()`, and the folio batch engine's fast-path check — these pages never call `folio_mc_copy()` and are never PTE-mapped, so they were never eligible for the two-phase unmap/move TLB-flush-batching machinery they were riding along in. `migrate_folio_done()`'s guard (the fourth) stays, since it's now shared correctness logic serving both the folio engine and `migrate_movable_ops_pages()`, not dead code. This is also a direct, scoped step toward the maintainer-acknowledged TODO on `migrate_movable_ops_page()` (mm/migrate.c:230-233, authored by David Hildenbrand, verified via `git blame` commit `b9ed00483d4cba`) — concentrating the special-casing into fewer places, without attempting the full page/folio type-system rework that TODO ultimately calls for.
3. **Split `struct migrate_pages_stats` into one local instance per engine, owned internally by that engine** (`migrate_hugetlbs()`, `migrate_movable_ops_pages()`, `migrate_folios()` — each declares and zeroes its own local, accumulates into it exclusively throughout its own body, and merges it into the caller-supplied `stats` pointer via a small `gather_migrate_pages_stats()` helper at each of its own return points), rather than the caller (`migrate_pages()`) declaring per-engine locals and merging after each call returns. `migrate_pages()` itself declares only the one grand-total `stats` and passes `&stats` directly into every engine call — it never declares a per-engine local or calls `gather_migrate_pages_stats()` itself. This replaces `migrate_pages_sync()`'s shadow `astats` struct and its asymmetric, comment-dependent manual field merge with straightforward accumulation, and gives every engine an identical external calling convention (`migrate_X(..., &stats, ...)`), matching the "each distinct migration mechanism gets its own function" goal all the way down into how each one manages its own accounting. `migrate_hugetlbs()` (unmodified in every other respect) gets this same small internal-local-plus-merge-at-return treatment as a minor, mechanical addition to this part's scope — its own retry/accounting *logic* is untouched, only where its stats writes land.
4. **Restructure `migrate_pages_sync()` into `migrate_folios()`**, unifying its async-pre-pass and per-folio-sync-fallback into one call site (nr_pass chosen by mode) plus a single early-return check with a field-selective stats merge matching the original exactly, and replacing its `while (!list_empty(from))` per-folio loop — which called the multi-pass `migrate_pages_batch()` on a singleton list — with a bounded, hugetlb-shaped `for (pass...)` loop calling a new primitive, `migrate_folio_sync_one()` (calling a new shared `migrate_folio_precheck()` — also used by `migrate_folios_batch()`'s own loop, so the pre-unmap fast paths exist in exactly one place — then the existing `migrate_folio_unmap()` once and `migrate_folio_move()` at most once, reusing their `__migrate_folio_record`/`__migrate_folio_extract` state handoff, and explicitly unwinding — via the existing `migrate_folio_undo_src()`/`migrate_folio_undo_dst()` helpers — a `-EAGAIN` from the move step rather than propagating its batching-era in-place-retry contract to a caller not equipped to honor it; this design is verified against the actual pre-batching `unmap_and_move()`, see Claude-Codex Deliberation). `migrate_pages_batch()` itself is renamed `migrate_folios_batch()` (pure rename; internally it's Part 1's already-extracted-`migrate_folios_unmap()` version, unchanged otherwise) and remains the executor for real multi-folio batching in `MIGRATE_ASYNC` mode.

## Acceptance Criteria

- AC-1: The unmap-phase loop currently inline in `migrate_pages_batch()` is extracted into a new named helper function `migrate_folios_unmap()`, following the parameter/naming style of the existing `migrate_folios_move()`/`migrate_folios_undo()` helpers in the same file.
  - Positive Tests (expected to PASS):
    - `grep -n "migrate_folios_unmap"` in `mm/migrate.c` finds a `static` helper function definition (return type may be `int`, an enum, or a small result struct — not mandated) distinct from the batch engine, whose return/outcome contract explicitly and unambiguously distinguishes the `goto move` vs `goto out` decision. This grep is a discovery aid only; the pass/fail determination is a code review confirming the contract is unambiguous.
    - The batch engine's body no longer contains the `switch (rc)` over -ENOMEM/-EAGAIN/0/default inline; that logic is reachable only via the new helper.
  - Negative Tests (expected to FAIL / be rejected):
    - A patch that only renames variables or reformats without introducing a new function boundary around the unmap loop.
    - A patch that extracts the loop but leaves the batch engine containing a duplicate/parallel copy of the same switch logic.

- AC-2: The `migrate_folios_unmap()` extraction preserves the batch engine's existing external behavior exactly, including its existing *non-uniform* handling of `try_to_unmap_flush()`: today, it's called exactly once per batch-engine invocation on the path that reaches the `move:` label, and is NOT called at all when the unmap phase hits `-ENOMEM` with an empty `unmap_folios` list (verified directly against mm/migrate.c:1964-1996). The refactor must reproduce this exact conditional behavior. Also covers: no change to `struct migrate_pages_stats` counters, list membership on return, `-EAGAIN` retry accumulation, THP-split-on-`-ENOMEM`-with-`MR_LONGTERM_PIN`, and deferred-split-folio handling, for both `MIGRATE_ASYNC` and non-async code paths.
  - Positive Tests (expected to PASS):
    - `mm/migrate.o` builds cleanly under the current `.config` (`CONFIG_MIGRATION=y`, `CONFIG_64BIT=y`, `CONFIG_NUMA=y`, `CONFIG_COMPACTION=y`) with no new compiler warnings. This is the minimum hard gate.
    - Where the environment supports it, `tools/testing/selftests/mm/migration.c`/`ksft_migration.sh` pass identically before and after. Best-effort supplementary verification, not a hard gate — record explicitly if the environment can't run them.
  - Negative Tests (expected to FAIL / be rejected):
    - Any change to `stats->nr_failed`/`nr_thp_failed`/`nr_failed_pages`/`nr_split`/`nr_thp_split` incrementing.
    - `try_to_unmap_flush()` called on the empty-`unmap_folios`/`-ENOMEM`/`goto out` path, or skipped on the `goto move` path.
    - Any change to the `MR_LONGTERM_PIN` retry-on-`-EAGAIN`-after-split special case.
  - AC-2.1: An independent (Codex) review confirms the extracted helper is a mechanical, behavior-preserving move of the existing loop body, with particular attention to the `try_to_unmap_flush()` conditional-call behavior.

- AC-3: `scripts/checkpatch.pl --strict` run against the resulting commit(s) (all four parts) introduces no new ERROR-level findings. New WARNING/CHECK-level findings on touched lines must be fixed or explicitly justified in the commit message.
  - Positive: `./scripts/checkpatch.pl --strict -g <commit-range>` reports 0 new ERROR-level findings.
  - Negative: Any new unfixed `ERROR:`-level finding.

- AC-4: Each part's **commit message** documents its rationale and (for Part 1) the seam it leaves for future batch-copy/offload work. Any **code comment** describes only *current* code structure — no references to external RFCs, DMA engines, or offload mechanisms in code comments.
  - Positive: Commit messages contain the relevant rationale; in-code comments describe current behavior only.
  - Negative: A code comment referencing specific external patches, RFC thread names, or hardware (e.g. "DCBM", "DMA offload").

- AC-5: `movable_ops` pages are filtered out of `from` in `migrate_pages()` into a new `migrate_movable_ops_pages()` pass — structured like `migrate_hugetlbs()`, including its own bounded `for (pass...)` retry loop (not a single-shot attempt) — before the `NR_MAX_BATCHED_MIGRATION` batch-splitting loop. After this change, `migrate_folio_unmap()`, `migrate_folio_move()`, and the folio engine's fast-path check contain no `page_has_movable_ops()` checks. `migrate_folio_done()`'s existing `page_has_movable_ops()` guard (mm/migrate.c:1203) is **kept, not removed** — it prevents incorrect `NR_ISOLATED_ANON`/`FILE` accounting for non-LRU pages, and `migrate_movable_ops_pages()`'s own success path now calls `migrate_folio_done()` too, so the guard is shared infrastructure serving two callers, not dead code specific to the branches being removed.
  - Positive Tests (expected to PASS):
    - `grep -c page_has_movable_ops mm/migrate.c` shows zero occurrences inside `migrate_folio_unmap()`, `migrate_folio_move()`, `migrate_folios_batch()`/`migrate_folios_unmap()` (occurrences in `migrate_folio_done()`, the existing movable_ops helper block mm/migrate.c:55-297, and the new pass, are expected and required).
    - `migrate_pages()` calls `migrate_movable_ops_pages()` before the batch-splitting loop, alongside the existing `migrate_hugetlbs()` call; the `again:` loop gains a `page_has_movable_ops()` safety-net check mirroring its existing `folio_test_hugetlb()` one, for stragglers that exhaust `migrate_movable_ops_pages()`'s own retry budget (mirrors exactly why the hugetlb safety-net check exists: `migrate_hugetlbs()` is called once, before `again:`, with a bounded internal retry budget — any folio still `-EAGAIN` when that budget is exhausted is never `list_move`d anywhere and stays on `from`, so `again:` must not sweep it into the folio engine).
    - `migrate_movable_ops_pages()`'s per-item primitive, `migrate_one_movable_ops_page()`, reuses the existing `migrate_movable_ops_page()` (mm/migrate.c:237) for the actual driver-dispatch step, and handles its own list placement (matching `unmap_and_move_hugetlb_folio()`'s pattern — `list_del()`+`migrate_folio_done()` on success, `list_move_tail()` to `ret_folios` on permanent failure, nothing on `-EAGAIN`); `migrate_movable_ops_pages()`'s own outer switch does only stats/retry bookkeeping, no list operations — mirroring `migrate_hugetlbs()`'s outer switch exactly.
    - On success, `migrate_one_movable_ops_page()` calls `folio_set_owner_migrate_reason(dst, reason)` and `folio_put(dst)` before finalizing `src` — matching `migrate_folio_move()`'s existing `out_unlock_both:` cleanup (mm/migrate.c:1421-1429) — so the migration's own reference to `dst` is correctly dropped, not leaked.
    - `migrate_one_movable_ops_page()`'s src-lock-contention handling reproduces the `PF_MEMALLOC` and `MIGRATE_SYNC_LIGHT && !folio_test_uptodate(src)` non-blocking-exit checks that today's shared code already applies to `movable_ops` pages before reaching their branch (verified at mm/migrate.c:1249-1258, which runs before the `page_has_movable_ops()` check at :1312).
    - `mm/migrate.o` builds cleanly (hard gate).
  - Negative Tests (expected to FAIL / be rejected):
    - Any `page_has_movable_ops()` check remaining in `migrate_folio_unmap()`, `migrate_folio_move()`, or the folio engine's fast-path check — or the guard being *removed* from `migrate_folio_done()`.
    - A single-shot (non-retrying) `migrate_movable_ops_pages()` — today, these pages already get retry benefit via shared `folio_trylock()` contention handling in the batch engine's own `nr_pass` loop; dropping that would be a real regression, not a refactor.
    - A `migrate_movable_ops_pages()` that duplicates `migrate_movable_ops_page()`'s driver-dispatch logic instead of calling it.
    - `movable_ops` pages silently dropped from `from` without being migrated, retried, or routed to `ret_folios` on failure.
    - `migrate_one_movable_ops_page()` succeeding without dropping `dst`'s migration reference (reference leak), or the outer loop redundantly moving a folio that the primitive already placed.
  - AC-5.1: External behavior for `movable_ops` pages is unchanged **except for one explicitly accepted simplification**: `isolate_movable_ops_page()`/`putback_movable_ops_page()` semantics, `PageMovableOpsIsolated` state transitions, and stats/`ret_folios` accounting on success and failure are identical before and after. The one deliberate exception (decided by the repository owner, not a residual gap): `migrate_movable_ops_pages()` always migrates in the caller's real `mode` from the first attempt — it does **not** replicate the async-pre-pass-then-real-mode-fallback policy that Part 4 gives regular folios. Today, `movable_ops` pages incidentally inherit that two-stage policy purely because they aren't yet separated from the regular-folio engine; `movable_ops` pages are primarily VM balloon/zsmalloc/offline pages, a use case that doesn't need the async-fast-path optimization built for bulk regular-folio migration throughput, so this plan does not replicate that complexity for them. `migrate_one_movable_ops_page()` also allocates a fresh `dst` on each outer-pass retry (mirroring `unmap_and_move_hugetlb_folio()`) rather than reusing the same `dst` across a driver-level `-EAGAIN` the way the batch engine's move-phase does for regular folios — same rationale, same acceptance.

    A separate point, not an exception but an invariant worth stating explicitly: `migrate_one_movable_ops_page()` does not reproduce `migrate_folio_unmap()`'s `folio_test_writeback(src)` wait/`-EBUSY` gate (mm/migrate.c:1266-1281), which today runs on every folio, including `movable_ops` ones, before the `page_has_movable_ops()` branch. This is not a behavior change: `page_has_movable_ops()` is exactly `PageMovableOps() && (PageOffline() || PageZsmalloc())` (include/linux/page-flags.h), and neither provider (`mm/balloon.c`'s `balloon_mops`, `mm/zsmalloc.c`'s `zsmalloc_mops`) — nor any other `__SetPageOffline()` caller in the tree (virtio_mem, vmw_balloon, hv_balloon, xen balloon, x86 coco/sev, powerpc memtrace, `mm_init.c`'s reserved-page marking) — ever calls into the writeback path; these pages are never part of a page cache or swap cache and so can never have `PG_writeback` set. The gate is unreachable, dead code for exactly this subset of folios today; omitting it is a correct simplification, not a gap.
    - Positive: Independent (Codex) review confirms the new pass reproduces the existing lock-src/lock-dst/`migrate_movable_ops_page()`/unlock/cleanup sequence with no change in isolate/putback semantics or failure-path cleanup, confirms the `dst` reference-drop and lock-acquisition-rule fixes above, and confirms the mode-sequencing/fresh-`dst`-per-retry differences are exactly the two accepted exceptions described here — no other divergence.
    - Negative: Review finds a change in `PageMovableOpsIsolated` clearing, a dst-folio leak, a src-folio left locked, changed failure-path stats accounting, or any divergence *beyond* the two explicitly accepted exceptions.
  - Note: there is no existing selftest coverage for `movable_ops` (balloon/zsmalloc/offline) migration in `tools/testing/selftests/mm/` (verified via grep) — this AC relies on the build gate and independent review, not a runnable regression test.

- AC-6: `struct migrate_pages_stats` accumulation is split per-engine, with each engine (`migrate_hugetlbs()`, `migrate_movable_ops_pages()`, `migrate_folios()`) owning its own local instance internally and merging it into the caller-supplied `stats` pointer via a `gather_migrate_pages_stats()` helper at each of its own return points — `migrate_pages()` passes `&stats` (the one grand-total instance it declares) directly into every engine call and never declares a per-engine local or calls `gather_migrate_pages_stats()` itself. This replaces `migrate_pages_sync()`'s shadow `astats` + manual asymmetric field merge with straightforward accumulation, encapsulated inside each engine rather than repeated three times at the call site.
  - Positive Tests (expected to PASS):
    - `migrate_pages()` declares exactly one `struct migrate_pages_stats stats`, zeroed once; every engine call (`migrate_hugetlbs(...)`, `migrate_movable_ops_pages(...)`, `migrate_folios(...)`, and the `split_folios` follow-up `migrate_folios_batch(...)` call within an `again:` iteration) takes `&stats` directly as its stats-target parameter. `migrate_pages()` contains zero `gather_migrate_pages_stats()` calls and zero per-engine local `struct migrate_pages_stats` declarations of its own.
    - `migrate_hugetlbs()`, `migrate_movable_ops_pages()`, and `migrate_folios()` each declare and zero their own internal local stats struct at the top of their body, accumulate into that local exclusively throughout, and call `gather_migrate_pages_stats(stats, &local)` immediately before each of their own `return` statements — one merge call per return point, covering every return path (including early `-ENOMEM`/error returns), so no return path skips the merge.
    - `migrate_hugetlbs()`'s retry/accounting *logic* (the bounded `for (pass...)` loop, `unmap_and_move_hugetlb_folio()` call, and the `switch (rc)` bookkeeping) is unchanged from mm/migrate.c:1638-1725 — the only diff is redirecting its existing `stats->` writes to a new internal local and adding the merge-at-return calls.
    - `gather_migrate_pages_stats()` additively merges all six fields (`nr_succeeded`, `nr_failed_pages`, `nr_thp_succeeded`, `nr_thp_failed`, `nr_thp_split`, `nr_split`); no other *whole-struct additive* merge logic exists elsewhere in the file (the field-selective `pass_stats`-into-`folio_local_stats` merge inside `migrate_folios()`, per AC-7, intentionally remains separate and is not a use of this helper), and `gather_migrate_pages_stats()` itself is called only from within the three engine functions, never from `migrate_pages()` directly.
  - Negative Tests (expected to FAIL / be rejected):
    - Any engine's internal local stats struct persisting across more than one call to that engine (e.g., a `static` or heap-allocated local instead of a fresh stack local per call) — this is what makes the merge-at-return pattern safe; violating it reintroduces the double-counting risk the design avoids.
    - A return path in any of the three engines that skips its `gather_migrate_pages_stats()` call, silently dropping that call's contribution to the grand total.
    - `migrate_pages()` itself declaring a `hugetlb_stats`/`movable_stats`/`folio_stats`-style local or calling `gather_migrate_pages_stats()` directly — that responsibility belongs entirely to each engine now.
    - Final reported counts (`*ret_succeeded`, `PGMIGRATE_SUCCESS`/`PGMIGRATE_FAIL`/`THP_MIGRATION_*` vmstat events, `rc_gather`) differing from today's values for any given input that does not trigger the AC-7 "Accepted retry-ordering and retry-economy exception" (i.e. neither a `-ENOMEM` nor a move-step `-EAGAIN` occurs anywhere during the folio sync fallback); AC-6's own merge mechanics (this criterion) are exact regardless of where the merge call physically lives (caller-side vs. callee-side), since the exception originates in AC-7's fallback-loop *ordering* and *retry-primitive shape*, not in a merge-arithmetic bug in `gather_migrate_pages_stats()` itself.

- AC-7: `migrate_pages_sync()` is renamed to `migrate_folios()` and restructured: one unified call to `migrate_folios_batch()` in `MIGRATE_ASYNC` mode (budget chosen by the caller's real mode via a single conditional expression using `NR_MAX_MIGRATE_PAGES_RETRY` for the pure-async case and `NR_MAX_MIGRATE_ASYNC_RETRY` for the pre-pass case — verified directly against mm/migrate.c:2138-2142, not the batch-size constant `NR_MAX_BATCHED_MIGRATION`, which would be a distinct bug), a single early-return covering both "caller wanted async" and "`-ENOMEM`" (both route survivors to `ret_folios`, merging **all** stats fields since the outcome is final either way; all other cases route survivors back into `from` for retry, merging **only** the always-final fields (`nr_succeeded`, `nr_thp_succeeded`, `nr_thp_split`, `nr_split`) plus the split-THP charge (`nr_thp_failed += nr_thp_split`) — mirroring the current `migrate_pages_sync()`'s exact field-selective merge, mm/migrate.c:2038-2053 — and explicitly *not* merging `nr_failed_pages`/raw `nr_thp_failed` from the pre-pass, since those folios are about to get a full second attempt below), and — replacing the current `while (!list_empty(from))` per-folio loop — a bounded `for (pass...)` loop, structured like `migrate_hugetlbs()`, calling a new primitive `migrate_folio_sync_one()` per folio per pass, with the loop itself owning all retry/exhaustion accounting directly.

  **Accepted retry-ordering and retry-economy exception**: this shared-pass, round-robin loop shape (pass-major, folio-minor — every folio in `from` gets one `migrate_folio_sync_one()` attempt per pass, in list order, before any folio gets a second attempt) is not behaviorally identical to today's `migrate_pages_sync()`, which is folio-major, pass-minor: it pops exactly **one** folio at a time (`list_move(from->next, &folios)`, mm/migrate.c:2061) into a private singleton list and drives *only that folio* through up to `NR_MAX_MIGRATE_SYNC_RETRY` attempts (via a nested `migrate_pages_batch()` call) before the next folio in `from` is even looked at — so a later-listed folio is never attempted at all until every earlier-listed folio has either succeeded, exhausted its own retries as `-EAGAIN`, or the earlier folio's own singleton call returns `-ENOMEM` (in which case `migrate_pages_sync()` returns immediately and every not-yet-reached later folio is left completely untouched — not attempted, not counted, in this call at all). The new round-robin loop, by contrast, interleaves every folio still in `from` within each pass, so a later-listed folio can be attempted — and can succeed or fail — in a pass where an earlier-listed folio is still mid-retry (`-EAGAIN`) and has not yet reached its own final outcome. Two concrete divergent scenarios follow from this, both accepted:
  - *Same-pass fold-in*: folio A returns `-EAGAIN` in the current pass and a later folio B in the same pass returns `-ENOMEM` — the new loop is fatal and folds A's accumulated retry into a final failure immediately, whereas today A would have already received its full singleton retry budget (and possibly succeeded) before B is ever reached.
  - *Cross-pass ordering*: folio B (earlier in list order) returns `-EAGAIN` in pass 0; folio A (later in list order) is attempted in that same pass 0 and reaches a final, non-retry outcome — succeeds, permanently fails via a precheck-handled path or `migrate_folio_sync_one()`'s own non-`-EAGAIN`/non-`-ENOMEM` failure branch, or splits successfully (charged via `nr_split`/`nr_thp_split`, moved to `split_folios` — the singleton `-ENOMEM`-then-split path in `migrate_folio_sync_one()`) (**not** an *exhausted-retry* failure — that specific accounting only fires once for every folio still marked `-EAGAIN` together, when the bounded `for` loop itself terminates, which cannot happen mid-loop while B is still being retried); folio B then returns `-ENOMEM` in a later pass. The new loop's final stats already count A's outcome (whichever of the above it was) from pass 0. Under today's sequential design, B (being earlier in list order) would still be mid-retry when its singleton call eventually returns `-ENOMEM`, and A — never reached — would remain untried, with no outcome counted at all, for this call.
  - *Returned-folio ordering (non-`-ENOMEM`, cosmetic)*: even when no `-ENOMEM` occurs, the physical order in which failed/not-migrated folios end up appended to `ret_folios` (and, transitively, spliced back into the caller's `from` list at the end of `migrate_pages()`, mm/migrate.c:2172) can differ between the two loop shapes, since folios are finalized in a different relative order (round-robin-per-pass vs. one-at-a-time-to-completion). Final *counts* and each individual folio's own *outcome* are unaffected — only the list order of the collected survivors can differ. This has no behavioral consequence: nothing downstream (`putback_movable_pages()` and all other consumers of the returned/not-migrated list) depends on, or documents, an ordering guarantee for that list. Accepted as part of the same exception, noted here only for completeness.
  - *Move-step `-EAGAIN` retry economy*: independent of any multi-folio ordering effect, `migrate_folio_sync_one()`'s handling of a *single* folio's own move-step `-EAGAIN` is itself not behaviorally identical to today's. Verified directly against `migrate_folios_move()` and its caller (mm/migrate.c:1727-1777, 1994-2012): today's `migrate_pages_batch()` — even when invoked on a singleton list by `migrate_pages_sync()`'s fallback — runs a dedicated move-phase `for (pass...)` loop that retries a `migrate_folio_move()` `-EAGAIN` **in place with the same already-allocated `dst`**, without ever re-unmapping the source or calling `get_new_folio()` again, for up to `nr_pass` attempts. `migrate_folio_sync_one()`, by contrast, fully unwinds on a move-step `-EAGAIN` — `migrate_folio_undo_dst()` calls `put_new_folio()`/`folio_put(dst)` (mm/migrate.c:1188-1197), genuinely releasing `dst`, not preserving it — so the *next* outer-pass retry of that same folio goes through `migrate_folio_precheck()` and `migrate_folio_unmap()` again: a fresh `get_new_folio()` call and a fresh unmap attempt, not a resumed in-place move retry. This means, for the identical single-folio move-`-EAGAIN`-then-retry scenario, the new design unconditionally repeats the full `get_new_folio()`/source-locking/unmap sequence on every retry pass — work today's in-place retry skips entirely, since its `dst` is already allocated and already locked — regardless of whether that repeated work ultimately succeeds. Two distinct consequences follow, not just one: (1) strictly more allocation/unmap/locking work per retry (a performance cost, not a correctness one, on its own), and (2) strictly more opportunities to hit *any* retryable or fatal outcome that this extra work can produce — not only `-ENOMEM` from a fresh `get_new_folio()` call returning `NULL`, but also a fresh `-EAGAIN` from `migrate_folio_unmap()` itself (e.g., lock contention on the newly-allocated `dst`, which today's design never needs to re-lock since its `dst` is already held locked across retries) — where today's design, reusing its one already-allocated-and-locked `dst`, would not have hit that outcome at all. A concrete case: today, `unmap` succeeds, `move` returns `-EAGAIN`, the same already-locked `dst` is retried and the move then succeeds — no second allocation or lock attempt ever happens. Under the new design, the same sequence instead releases `dst` entirely, and the next pass's fresh `get_new_folio()` + fresh lock/unmap attempt could itself return `-ENOMEM` or a fresh `-EAGAIN` where today's retry-in-place would have succeeded outright. This is accepted on the same grounds as the ordering divergence above: it reproduces the historically-verified pre-batching `__unmap_and_move()` behavior (see "Historical verification" below), which also never preserved in-place retry state across a failed attempt — the batching-era same-`dst`-retry optimization is not carried forward for the sync-fallback primitive, consistent with the repository owner's earlier decision to restore pre-batching behavior for this path rather than reproduce the batching engine's every internal optimization. `migrate_folio_sync_one()` remaining a fully self-contained, single-attempt primitive (no state straddling separate calls) is also what keeps it structurally parallel to `unmap_and_move_hugetlb_folio()`.

  These divergences have three distinct triggers, not one: the *substantive outcome/count* divergences (same-pass fold-in, cross-pass ordering) are confined to when a `-ENOMEM` is reached by *some* folio in `from` while another folio's outcome has not yet been finalized under the old model; the *retry-economy* divergence is triggered by any move-step `-EAGAIN` on a folio's own attempt — with or without any other folio being involved, and regardless of whether the subsequent retry ultimately succeeds, since the extra unmap/lock/allocate work (and its own chance of a fresh `-EAGAIN` or `-ENOMEM`) happens every time a move-step `-EAGAIN` occurs, not only when it happens to lead to a final `-ENOMEM`; and the *cosmetic returned-list-ordering* effect is broader than either of the above — it can arise from **any** kind of `-EAGAIN` interleaving across folios (unmap-phase, precheck, or move-phase alike), not only a move-step `-EAGAIN` or a `-ENOMEM`, simply because the round-robin loop finalizes folios in a different relative order than the old sequential-to-completion loop whenever more than one folio needs more than one attempt — it is *not* scoped under the `-ENOMEM`/move-step-`-EAGAIN` triggers above and carries no correctness or count implication regardless of what triggers it. Outside of a `-ENOMEM` occurring at all *and* outside of any move-step `-EAGAIN` occurring during the sync fallback, **outcomes and counts** are identical between the two loops for every folio (list order of any returned/failed folios is the sole exception, per the cosmetic bullet, and never carries a count/outcome difference on its own). This was found and confirmed by Codex rounds 4-9 (see Claude-Codex Deliberation) and is accepted by the repository owner as an intentional, documented behavior change, not a bug to fix — `migrate_hugetlbs()` already uses this exact pass-major/folio-minor shape today, so `migrate_folios()` adopting it for consistency, and `migrate_folio_sync_one()` staying a clean self-contained primitive, is judged worth the divergences it introduces, all of which only arise on the already-uncommon retry path (a move-step `-EAGAIN` or an allocation failure), not during normal-operation migration.

  On the retry-economy divergence specifically: the full unwind between move-step retries also restores migration PTEs and unlocks `src` (via `migrate_folio_undo_src()`) before the next pass's fresh attempt, in addition to releasing `dst` — so a retried folio's source-side state (mapped/locked) genuinely cycles through unmapped→remapped→unmapped again across retries, unlike today's design, where `src` stays unmapped and `dst` stays locked continuously across in-place move retries. This has no effect on final per-folio *success-path* accounting: `migrate_folio_done()`'s NUMA/vmstat/memcg success counters (`count_vm_numa_events()`, `count_memcg_events()`, etc.) only ever fire once, on the attempt that actually succeeds, exactly as today — the extra remap/unmap cycling only affects work done on the *failed* intermediate attempts, which were never counted as success in either design.

  `migrate_folio_sync_one()` is **not** a naive back-to-back call of `migrate_folio_unmap()` and `migrate_folio_move()`. It has two distinct correctness obligations, discovered through review and independently verified before being incorporated:

  1. **The pre-unmap fast paths must be preserved, not skipped.** `migrate_pages_batch()`'s loop applies three checks before ever calling `migrate_folio_unmap()` — deferred-split-list handling, unsupported-THP splitting, and the `folio_ref_count(folio) == 1` "freed under us" fast path (mm/migrate.c:1838-1917) — plus special `-ENOMEM` handling after unmap (large-folio split retry, including the `MR_LONGTERM_PIN` `-EAGAIN` special case). A `migrate_folio_sync_one()` that composed only `migrate_folio_unmap()`+`migrate_folio_move()` would silently skip all of this for the sync-fallback path. Rather than duplicating this logic (risking it drifting out of sync with the batch engine's copy), the pre-unmap checks are extracted into a new shared helper, `migrate_folio_precheck()`, called by **both** `migrate_folios_batch()`'s loop and `migrate_folio_sync_one()`. This is a small additional change to `migrate_folios_unmap()` (Part 1) made as part of this milestone, not a reopening of Part 1's already-converged external behavior — see Dependencies and Sequence. The post-unmap `-ENOMEM` handling is *not* shared verbatim: the batch engine's version is entangled with batch-abandon logic (`goto move`/`goto out` depending on whether other folios in the batch already unmapped) that has no meaning for a single folio — for a true singleton, `unmap_folios` is necessarily empty when `-ENOMEM` occurs, so the existing code's own logic already degenerates to "clean up and return" in that case. `migrate_folio_sync_one()` therefore implements the genuinely-simpler singleton form of the same policy (try split with the `MR_LONGTERM_PIN` retry case, else report failure) directly, rather than duplicating the more complex batch form.
  2. **`migrate_folio_move()`'s `-EAGAIN` path must be explicitly unwound, not propagated.** It is a batching-era contract (mm/migrate.c:1448-1451) — deliberately leaves `src`/`dst` locked and `dst` re-queued, expecting an in-place retry on the *same* objects, introduced when `unmap_and_move()` was split into `_unmap()`/`_move()` (commit `64c8902ed4418`) specifically to enable multi-folio batching. Verified directly against the pre-split `__unmap_and_move()` (parent of commit `8a27e6b478415`, "migrate_pages: restrict number of pages to migrate in batch" — the commit that introduced the batch engine and `migrate_pages_batch()` in the first place): on **any** non-success outcome, including the identical refcount-race `-EAGAIN` (from `__migrate_folio()`'s `folio_ref_count(src) != expected_count` check, mm/migrate.c:877), the old combined function unconditionally unlocked both folios and released `dst`, with no in-place-retry state at all, and never used deferred/batched TLB flushing (`try_to_migrate(src, 0)`, no `TTU_BATCH_FLUSH`, ever) — the correct, shipped design for exactly this single-folio scenario, for years, before batching existed. `migrate_folio_sync_one()` therefore explicitly unwinds a `migrate_folio_move()` `-EAGAIN` — via `__migrate_folio_extract()`, an explicit `list_del(&dst->lru)` (required: `migrate_folio_move()`'s own `-EAGAIN` path re-links `dst->lru` into the caller's list via `list_add()`, and `migrate_folio_undo_dst()` does not unlink it — omitting this leaves `dst` linked into a stack-local list that is about to go out of scope), then `migrate_folio_undo_src()`/`migrate_folio_undo_dst()` (`ret=NULL` so the folio stays in place for the outer loop's next pass) — rather than propagating the batching-era "resume in place" contract to a caller that isn't equipped to honor it. No `try_to_unmap_flush()` call is needed: `migrate_folio_sync_one()` is only ever invoked in non-`MIGRATE_ASYNC` mode, where `migrate_folio_unmap()` never requests `TTU_BATCH_FLUSH` (mm/migrate.c:1338) in the first place, so nothing is ever deferred to flush — confirmed by the pre-batching code never calling an equivalent flush either.

  `migrate_folio_sync_one()` owns *all* stats accounting for every outcome it can produce (precheck-handled, unmap-phase, and move-phase alike) via a passed-in `stats` pointer and an `int *nr_failed` out-parameter; its own return value means exactly one thing to its caller — `-EAGAIN` means retry this folio, anything else means it's done (fully accounted for already). This avoids a split-brain design where both the primitive and the outer loop try to interpret raw `migrate_folio_unmap()`/`migrate_folio_move()` return codes independently.

  - Positive Tests (expected to PASS):
    - Exactly one call site invokes `migrate_folios_batch(..., MIGRATE_ASYNC, ...)` within `migrate_folios()`, with `nr_pass` selected via a mode-dependent expression (`NR_MAX_MIGRATE_PAGES_RETRY` for async, `NR_MAX_MIGRATE_ASYNC_RETRY` otherwise), not duplicated across two call sites.
    - The async-pre-pass merge is field-selective as described above, not a wholesale merge of all six stats fields.
    - `migrate_folio_precheck()` exists as a single shared function, called by both `migrate_folios_batch()`'s loop (replacing its inline copy) and `migrate_folio_sync_one()` — not duplicated logic in two places.
    - The per-folio fallback is a `for (pass = 0; pass < NR_MAX_MIGRATE_SYNC_RETRY && retry; pass++) { list_for_each_entry_safe(...) { ... } }` loop, not a `while (!list_empty(from))` loop; `nr_retry_pages`/`thp_retry` for the final "exhausted" charge are derived from the folio's own state (`folio_nr_pages()`/`folio_test_pmd_mappable()`) at the point of a `-EAGAIN` return, not threaded through return values.
    - `migrate_folio_sync_one()`'s `-EAGAIN`-from-move unwind includes `list_del(&dst->lru)` before `migrate_folio_undo_dst()`.
    - `migrate_folio_sync_one()`'s singleton `-ENOMEM` handler applies the `nosplit = (reason == MR_NUMA_MISPLACED)` guard (mm/migrate.c:1828) before attempting a large-folio split, matching the batch engine exactly.
    - `-ENOMEM` from `migrate_folio_sync_one()` is treated as fatal by the outer fallback loop: it immediately folds the current pass's already-accumulated `-EAGAIN` stragglers (`nr_retry_pages`/`thp_retry`) into final failure counts and returns `-ENOMEM`, leaving the triggering folio and everything not yet visited in `from` untouched for the caller's unconditional `list_splice_tail_init()` — mirroring both the batch engine's own `-ENOMEM`-abort fold-in (mm/migrate.c:1960-1962) and today's `migrate_pages_sync()`'s immediate `return rc` on `-ENOMEM`.
    - `migrate_folio_precheck()` calls `cond_resched_tasks_rcu_qs()` (not plain `cond_resched()`), matching mm/migrate.c:1843, and is the only place either caller needs to call it.
    - Final success/failure counts, including THP-specific counters (`nr_thp_succeeded`, `nr_thp_failed`, `nr_thp_split`), for a given input are bit-identical to today's `migrate_pages_sync()` output, **except** for the divergences described in AC-7's "Accepted retry-ordering and retry-economy exception" above: the same-pass fold-in case and the cross-pass case (both `-ENOMEM`-triggered: a later-listed folio reaches a final outcome in the new loop before an earlier-listed, still-retrying folio's own later `-ENOMEM`, when today's design would never have reached that later folio at all), and the move-step retry-economy case (triggered by any move-step `-EAGAIN`, not only when it results in `-ENOMEM`: a single folio's own move-retry repeats allocation/lock/unmap work today's in-place retry skips, and can hit a fresh `-ENOMEM` or fresh `-EAGAIN` on that repeated work where today's same-`dst` in-place retry would have succeeded outright). Equivalence for inputs that trigger none of these — no `-ENOMEM` anywhere in the sync fallback, and no move-step `-EAGAIN` on any folio — is verified by AC-7.1.
  - Negative Tests (expected to FAIL / be rejected):
    - A folio counted as both failed and succeeded for the same migration attempt.
    - `migrate_folio_sync_one()` propagating a `migrate_folio_move()` `-EAGAIN` upward without first unwinding it (locks/list state), or unwinding it without the `list_del(&dst->lru)` step.
    - The sync fallback silently skipping deferred-split handling, unsupported-THP splitting, the refcount==1 fast path, the `MR_NUMA_MISPLACED` no-split guard, or `-ENOMEM` large-folio-split handling for any folio that would have hit one of those paths in the batch engine.
    - The sync fallback loop continuing to process further folios (or further passes) after a `-ENOMEM` return from `migrate_folio_sync_one()`, or discarding — rather than folding into final failure counts — the current pass's already-accumulated `-EAGAIN` stragglers when aborting on `-ENOMEM`.
    - THP-specific counters (`nr_thp_succeeded`/`nr_thp_failed`/`nr_thp_split`) not being updated by the sync-fallback path.
    - `nr_pass=1` (or similar) being passed to `migrate_folios_batch()` repeatedly from an outer loop as an alternative retry mechanism — this was tried and found to double-count, since the low-level engine's own "exhausted this pass" accounting fires immediately with a 1-pass budget, before the outer loop gets a chance to retry (see Claude-Codex Deliberation).
  - AC-7.1: An independent (Codex) review traces the exact accounting semantics against the current `migrate_pages_sync()`+`migrate_pages_batch()` (mm/migrate.c:1811-2072) line by line, confirming: (a) the split-THP charge (`stats->nr_thp_failed` credited from `nr_thp_split`) is preserved via the field-selective merge, (b) no folio can be counted as both failed and succeeded, (c) the `-ENOMEM`/async-mode early-return path's stats are unaffected by the field-selective merge (mirrors today's unconditional merge on that path), (d) `migrate_folio_sync_one()`'s `-EAGAIN` unwind correctly releases every reference/lock/list-linkage that `migrate_folio_unmap()`+`migrate_folio_move()` acquired, with no leak and no residual locked or linked state visible to the next pass, (e) `migrate_folio_precheck()`'s behavior is identical to the inline logic it replaces in `migrate_folios_unmap()`, for both callers, including the `cond_resched_tasks_rcu_qs()` side effect, (f) the singleton `-ENOMEM` handling in `migrate_folio_sync_one()` is a correct degenerate case of the batch engine's version including the `MR_NUMA_MISPLACED` `nosplit` guard, (g) the fallback loop's `-ENOMEM` handling is fatal and correctly folds in the current pass's already-accumulated `-EAGAIN` stragglers before returning, with no folio reachable that is both left for retry and silently dropped or double-counted — covering both (i) the same-pass case, where one folio returns `-EAGAIN` and a later folio in the same pass returns `-ENOMEM`, and (ii) the cross-pass case, where a later-listed folio is attempted and reaches its own final, non-retry outcome — success, or a permanent failure via a precheck-handled path or `migrate_folio_sync_one()`'s own non-`-EAGAIN`/non-`-ENOMEM` failure branch, but **not** an exhausted-retry failure, since that accounting only fires once, for every still-retrying folio together, when the bounded `for` loop itself terminates, and so cannot have already happened to a specific folio while the loop is still running to reach a later folio's `-ENOMEM` — in an earlier pass, and only afterward does an earlier-listed folio, still mid-retry, return `-ENOMEM` in a subsequent pass — confirming that folio is counted exactly once (as succeeded or failed, per its own outcome) and is neither re-touched by, nor folded into, the later `-ENOMEM` folio's fold-in. This point establishes only that each folio is **counted exactly once under the new loop's own accounting** (no leak, no duplication, no folio finalized in one pass being re-processed by a later pass's fold-in), and explicitly does **not** claim that any individual folio had an **equivalent retry opportunity, or was reached/not-reached in the same order, as today's `migrate_pages_sync()`**: under the new round-robin shape, a folio may be folded into final failure after fewer attempts than its full singleton budget (same-pass case), or a later-listed folio may be attempted and even succeed in a pass where today's sequential design would never have reached it at all because an earlier-listed folio was still consuming its own private retry budget (cross-pass case). Both retry-opportunity/ordering divergences are the accepted exception documented in AC-7 and are out of scope for this point, which is purely an accounting-correctness check. (h) `migrate_folio_sync_one()`'s move-step `-EAGAIN` unwind — already confirmed leak-free by (d) — releases `dst` rather than preserving it for an in-place retry, unlike today's `migrate_folios_move()`/`migrate_pages_batch()` move-phase pass loop (mm/migrate.c:1727-1777, 1994-2012), which retries a move-step `-EAGAIN` against the *same*, already-allocated-and-locked `dst` without a fresh `get_new_folio()` call, fresh `dst` locking, or re-unmap. This is a real behavioral divergence — not an accounting defect — accepted per AC-7's exception: a folio's own move-retry now unconditionally repeats the full allocate/lock/unmap sequence on every retry pass, so it can hit *any* outcome that sequence can produce — not only `-ENOMEM` from a fresh `get_new_folio()` returning `NULL`, but also a fresh `-EAGAIN` from `migrate_folio_unmap()` itself (e.g., contending for the lock on the newly-allocated `dst`, a lock today's design never needs to reacquire since its one `dst` stays held across retries) — where today's in-place retry, reusing its already-allocated-and-locked `dst`, would not have hit that outcome at all. (d)'s no-leak/no-residual-state claim and (h)'s retry-economy disclaimer are compatible: the unwind is correct and complete (nothing leaks), it simply repeats work — and the failure/retry exposure that work carries — that today's optimized in-place retry skips entirely.
    - Positive: Review confirms accounting correctness (no leak/loss/double-count) for all eight points, and confirms the six points other than (g) and (h) additionally hold full semantic equivalence with today's `migrate_pages_sync()`.
    - Negative: Review finds any accounting divergence, a lock/reference/list-linkage leak, residual in-place-retry state reachable from the outer loop, a precheck/`-ENOMEM`/`nosplit` behavior mismatch, a folio silently lost/double-counted around a `-ENOMEM` abort, or any point *other than* (g) and (h) where retry-opportunity/retry-economy/output equivalence does not hold.

## Path Boundaries

### Upper Bound (Maximum Acceptable Scope)

All four parts as described (AC-1 through AC-7.1), including addressing any newly-surfaced checkpatch findings and documenting each part's rationale. This is the full scope of this plan.

### Lower Bound (Minimum Acceptable Scope)

Identical to the Upper Bound. This is a single, coherent redesign where the parts depend on each other for their stated rationale (Part 3's per-engine local-plus-merge-at-return pattern for `migrate_movable_ops_pages()` has no meaning without Part 2 introducing that function; Part 4's uniform retry-loop shape is the direct answer to the readability problem Part 2 also addresses) — see Dependencies and Sequence for the one genuine ordering constraint (Part 1 before Part 2) versus parts that must land together (Part 3 with Part 4).

### Allowed Choices

- Can use: the specific function names used throughout this plan (`migrate_folios_unmap`, `migrate_movable_ops_pages`, `migrate_one_movable_ops_page`, `gather_migrate_pages_stats`, `migrate_folios`, `migrate_folio_sync_one`, `migrate_folios_batch`) are illustrative — alternative names consistent with existing file conventions are acceptable; an enum or small result struct instead of a bare `int` for `migrate_folios_unmap()`'s move-vs-out outcome, if clearer than an out-parameter.
- Cannot use: any change to `migrate_pages()`'s **public signature** (include/linux/migrate.h) — its internal body is fully in scope for this redesign, superseding the original narrower plan's broader "don't touch `migrate_pages()` at all" framing; introduction of any batch-copy, DMA, or hardware-offload logic itself; introduction of new Kconfig options, new files under drivers/, or new dependencies; a `struct migrate_control`-style parameter consolidation (`get_new_folio`/`put_new_folio`/`private`/`mode`/`reason` bundled into one struct) — considered and explicitly deferred as a separate follow-up plan, see DEC-1, unaffected by and orthogonal to this redesign's stats-splitting (which goes the opposite direction: more separate structs, not fewer); any attempt to solve the broader "movable_ops pages will not be folios in the future" TODO (mm/migrate.c:230-233) beyond isolating them into their own pass.

> **Note on Deterministic Designs**: This redesign was fully specified through iterative discussion, including working through and correcting two real bugs found along the way (the `try_to_unmap_flush()` conditionality error in the original Part 1 draft, and a stats double-counting bug in an early version of Part 4 caught before it was written down). The design is intentionally fixed; Upper/Lower Bound converge to the same scope.

## Feasibility Hints and Suggestions

> **Note**: This section is for reference and understanding only. These are conceptual suggestions, not prescriptive requirements — though unlike a typical "one possible approach" sketch, this one reflects a design that was specifically debugged during planning (see Known Risks for the bugs that were found and fixed in earlier drafts).

### Part 1: `migrate_folios_unmap()`

```c
static int migrate_folios_unmap(struct list_head *from,
                new_folio_t get_new_folio, free_folio_t put_new_folio,
                unsigned long private, enum migrate_mode mode,
                enum migrate_reason reason, int nr_pass,
                struct list_head *unmap_folios, struct list_head *dst_folios,
                struct list_head *ret_folios, struct list_head *split_folios,
                struct migrate_pages_stats *stats,
                int *rc_saved, bool *need_move)
{
        /* body = today's unmap-phase loop, moved verbatim. Sets *need_move
         * to distinguish the "goto move" vs "goto out" outcome explicitly,
         * so the caller's branch reproduces today's control flow (including
         * that try_to_unmap_flush() runs only on the *need_move path). */
}
```

`migrate_folios_batch()` (Part 1's renamed `migrate_pages_batch()`) then reads as: call `migrate_folios_unmap()`; branch on the outcome exactly as today's `goto move`/`goto out` does; run the move-phase retry loop calling `migrate_folios_move()`; call `migrate_folios_undo()` on the "out" branch.

### Part 2: `migrate_movable_ops_pages()`

```c
/* Mirrors unmap_and_move_hugetlb_folio()'s shape: a single-attempt
 * primitive that handles its own list placement (list_del()+
 * migrate_folio_done() on success, list_move_tail() to ret_folios on
 * permanent failure, nothing on -EAGAIN), so the outer loop's switch
 * only needs to do stats/retry bookkeeping. */
static int migrate_one_movable_ops_page(struct folio *src, new_folio_t get_new_folio,
                free_folio_t put_new_folio, unsigned long private,
                enum migrate_mode mode, enum migrate_reason reason,
                struct list_head *ret_folios)
{
        struct folio *dst;
        int rc = -EAGAIN;

        dst = get_new_folio(src, private);
        if (!dst)
                return -ENOMEM;

        /* Same non-blocking-exit rules migrate_folio_unmap() already
         * applies to movable_ops pages today, before reaching their
         * branch (mm/migrate.c:1249-1258). */
        if (!folio_trylock(src)) {
                if (mode == MIGRATE_ASYNC)
                        goto out_put_dst;
                if (current->flags & PF_MEMALLOC)
                        goto out_put_dst;
                if (mode == MIGRATE_SYNC_LIGHT && !folio_test_uptodate(src))
                        goto out_put_dst;
                folio_lock(src);
        }

        /* migrate_folio_unmap() also applies a folio_test_writeback(src)
         * wait/-EBUSY gate here today (mm/migrate.c:1266-1281) before
         * reaching the page_has_movable_ops() branch. Not reproduced:
         * page_has_movable_ops() is exactly PageMovableOps() &&
         * (PageOffline() || PageZsmalloc()) (include/linux/page-flags.h),
         * and neither provider (mm/balloon.c, mm/zsmalloc.c) -- nor any
         * other __SetPageOffline() caller (virtio_mem, vmw_balloon,
         * hv_balloon, xen balloon, coco/sev, memtrace, mm_init reserved
         * pages) -- ever calls into the writeback path; these pages are
         * never page-cache- or swap-backed, so PG_writeback can never be
         * set on one. The gate is unreachable dead code for this exact
         * subset of folios today; omitting it here is not a behavior
         * change. See AC-5.1. */
        if (!folio_trylock(dst))
                goto out_unlock_src;

        rc = migrate_movable_ops_page(&dst->page, &src->page, mode);

        folio_unlock(dst);
        if (!rc) {
                /* Matches migrate_folio_move()'s out_unlock_both: cleanup
                 * (mm/migrate.c:1421-1429): drop the migration's own
                 * reference to dst now that ownership has transferred. */
                folio_set_owner_migrate_reason(dst, reason);
                folio_put(dst);
                list_del(&src->lru);
                folio_unlock(src);
                migrate_folio_done(src, reason);
                return 0;
        }

out_unlock_src:
        folio_unlock(src);
out_put_dst:
        if (put_new_folio)
                put_new_folio(dst, private);
        else
                folio_put(dst);

        if (rc != -EAGAIN)
                list_move_tail(&src->lru, ret_folios);

        return rc;
}

static int migrate_movable_ops_pages(struct list_head *from,
                new_folio_t get_new_folio, free_folio_t put_new_folio,
                unsigned long private, enum migrate_mode mode,
                enum migrate_reason reason,
                struct migrate_pages_stats *stats,
                struct list_head *ret_folios)
{
        /* local_stats is this engine's own scratch: every write in this
         * function's body targets it exclusively, and it is merged into
         * the caller-supplied `stats` via gather_migrate_pages_stats()
         * at each return point below -- the caller never sees a
         * partially-accumulated intermediate state. */
        struct migrate_pages_stats local_stats;
        int retry = 1, nr_failed = 0, nr_retry_pages = 0, pass;
        struct folio *folio, *folio2;
        int rc, nr_pages;

        memset(&local_stats, 0, sizeof(local_stats));

        for (pass = 0; pass < NR_MAX_MIGRATE_PAGES_RETRY && retry; pass++) {
                retry = 0;
                nr_retry_pages = 0;

                list_for_each_entry_safe(folio, folio2, from, lru) {
                        if (!page_has_movable_ops(&folio->page))
                                continue;

                        nr_pages = folio_nr_pages(folio);
                        cond_resched();

                        rc = migrate_one_movable_ops_page(folio, get_new_folio,
                                        put_new_folio, private, mode, reason,
                                        ret_folios);
                        switch (rc) {
                        case -ENOMEM:
                                nr_failed++;
                                local_stats.nr_failed_pages += nr_pages + nr_retry_pages;
                                gather_migrate_pages_stats(stats, &local_stats);
                                return -ENOMEM;
                        case -EAGAIN:
                                retry++;
                                nr_retry_pages += nr_pages;
                                break;
                        case 0:
                                local_stats.nr_succeeded += nr_pages;
                                break;
                        default:
                                nr_failed++;
                                local_stats.nr_failed_pages += nr_pages;
                                break;
                        }
                }
        }
        nr_failed += retry;
        local_stats.nr_failed_pages += nr_retry_pages;

        gather_migrate_pages_stats(stats, &local_stats);
        return nr_failed;
}
```

This is structured identically to `migrate_hugetlbs()`/`unmap_and_move_hugetlb_folio()` (mm/migrate.c:1638-1725, 1480-1589): bounded outer retry-pass loop, inner filtered scan, single-attempt per-item primitive that owns its own list placement, outer switch doing only stats/retry bookkeeping (no list operations — matching `migrate_hugetlbs()`'s own outer switch exactly, which also does none).

### Part 3: stats merge helper

```c
static void gather_migrate_pages_stats(struct migrate_pages_stats *dst,
                                       const struct migrate_pages_stats *src)
{
        dst->nr_succeeded     += src->nr_succeeded;
        dst->nr_failed_pages  += src->nr_failed_pages;
        dst->nr_thp_succeeded += src->nr_thp_succeeded;
        dst->nr_thp_failed    += src->nr_thp_failed;
        dst->nr_thp_split     += src->nr_thp_split;
        dst->nr_split         += src->nr_split;
}
```

This helper is called from *inside* each of the three engine functions, at each of their own return points — never from `migrate_pages()` directly (see the top-level sketch below, which passes `&stats` straight into every engine call). `migrate_movable_ops_pages()` (Part 2, above) already shows the pattern: a local `struct migrate_pages_stats local_stats` declared and zeroed at the top of the function, written to exclusively throughout the body, and merged into the caller-supplied `stats` via `gather_migrate_pages_stats(stats, &local_stats)` immediately before each `return`.

`migrate_hugetlbs()` (mm/migrate.c:1638-1725, otherwise entirely unmodified by this redesign) receives the identical treatment as a small, mechanical part of this milestone: its existing `stats->nr_failed_pages += ...` / `stats->nr_succeeded += ...` writes are redirected to a new internal `local_stats`, and a `gather_migrate_pages_stats(stats, &local_stats)` call is added immediately before its two existing `return` statements (the early `-ENOMEM` return and the final `return nr_failed`). No change to its retry-pass loop, its call to `unmap_and_move_hugetlb_folio()`, or its `switch (rc)` bookkeeping — this is purely redirecting where its stats writes land, so it takes the master `stats` pointer as a parameter with the same external contract as the other two engines instead of writing into it directly as it does today.

`migrate_folios()` (Part 4, below) follows the same pattern for its own outward-facing accounting, layered on top of its existing internal `pass_stats` scratch (which remains a separate, field-selective concern — see Part 4's code and rationale).

### Part 4: `migrate_folios()` and `migrate_folio_sync_one()`

```c
/* The pre-unmap fast paths shared by migrate_folios_batch()'s loop and
 * migrate_folio_sync_one(): deferred-split handling, unsupported-THP
 * splitting, and the refcount==1 "freed under us" fast path (mirrors
 * mm/migrate.c:1838-1917 exactly), including the cond_resched_tasks_rcu_qs()
 * call both callers need -- calling it here means it exists in exactly one
 * place instead of being duplicated at each call site. page_has_movable_ops()
 * no longer needs checking here once Part 2 lands -- those folios never
 * reach this loop.
 *
 * Returns true if the folio was fully handled (stats/list placement
 * already updated; caller must not call migrate_folio_unmap()), false
 * if the caller should proceed to unmap. */
static bool migrate_folio_precheck(struct folio *folio,
                enum migrate_mode mode, enum migrate_reason reason,
                struct list_head *split_folios, struct list_head *ret_folios,
                struct migrate_pages_stats *stats, int *nr_failed,
                bool *is_thp, int *nr_pages)
{
        cond_resched_tasks_rcu_qs();

        *is_thp = folio_test_pmd_mappable(folio);
        *nr_pages = folio_nr_pages(folio);

        if (*nr_pages > 2 &&
            !list_empty(&folio->_deferred_list) &&
            folio_test_partially_mapped(folio)) {
                if (!try_split_folio(folio, split_folios, mode)) {
                        (*nr_failed)++;
                        stats->nr_thp_failed += *is_thp;
                        stats->nr_thp_split += *is_thp;
                        stats->nr_split++;
                        return true;
                }
        }

        if (!thp_migration_supported() && *is_thp) {
                (*nr_failed)++;
                stats->nr_thp_failed++;
                if (!try_split_folio(folio, split_folios, mode)) {
                        stats->nr_thp_split++;
                        stats->nr_split++;
                        return true;
                }
                stats->nr_failed_pages += *nr_pages;
                list_move_tail(&folio->lru, ret_folios);
                return true;
        }

        if (folio_ref_count(folio) == 1) {
                folio_clear_active(folio);
                folio_clear_unevictable(folio);
                list_del(&folio->lru);
                migrate_folio_done(folio, reason);
                stats->nr_succeeded += *nr_pages;
                stats->nr_thp_succeeded += *is_thp;
                return true;
        }

        return false;
}

/* migrate_folio_move()'s -EAGAIN path (mm/migrate.c:1448-1451) is a
 * batching-era contract: it leaves src/dst locked and dst re-queued,
 * expecting an in-place retry on the SAME objects. That's necessary for
 * the batch engine's own move-phase retry loop, but wrong for a
 * single-attempt caller. Verified against the pre-batching
 * __unmap_and_move() (parent of the commit that introduced
 * migrate_pages_batch(), "migrate_pages: restrict number of pages to
 * migrate in batch"): it handled the identical refcount-race -EAGAIN by
 * unconditionally unlocking both folios and releasing dst -- no in-place
 * retry state, ever -- which is exactly what a single-folio caller like
 * this one should still do. No try_to_unmap_flush() is needed here: this
 * function only ever runs in non-async mode, where migrate_folio_unmap()
 * never requests TTU_BATCH_FLUSH (mm/migrate.c:1338), so nothing is ever
 * deferred to flush.
 *
 * Owns all stats accounting for every outcome; return value means only
 * one thing to the caller: -EAGAIN = retry this folio, anything else =
 * done (already fully accounted for). */
static int migrate_folio_sync_one(struct folio *folio, new_folio_t get_new_folio,
                free_folio_t put_new_folio, unsigned long private,
                enum migrate_mode mode, enum migrate_reason reason,
                struct list_head *split_folios, struct list_head *ret_folios,
                struct migrate_pages_stats *stats, int *nr_failed)
{
        struct folio *dst = NULL;
        LIST_HEAD(dst_list);
        int rc, old_folio_state, nr_pages;
        struct anon_vma *anon_vma;
        bool is_thp, is_large, nosplit;

        if (migrate_folio_precheck(folio, mode, reason, split_folios,
                        ret_folios, stats, nr_failed, &is_thp, &nr_pages))
                return 0;       /* handled -- not a "retry me" signal */

        is_large = folio_test_large(folio);
        /* Matches mm/migrate.c:1828 exactly: NUMA-misplaced migration
         * never splits on allocation failure. */
        nosplit = (reason == MR_NUMA_MISPLACED);
        rc = migrate_folio_unmap(get_new_folio, put_new_folio, private,
                        folio, &dst, mode, ret_folios);
        switch (rc) {
        case -ENOMEM:
                (*nr_failed)++;
                stats->nr_thp_failed += is_thp;
                if (is_large && !nosplit) {
                        int ret = try_split_folio(folio, split_folios, mode);

                        if (!ret) {
                                stats->nr_thp_split += is_thp;
                                stats->nr_split++;
                                return 0;
                        } else if (reason == MR_LONGTERM_PIN && ret == -EAGAIN) {
                                (*nr_failed)--;
                                stats->nr_thp_failed -= is_thp;
                                return -EAGAIN;
                        }
                }
                stats->nr_failed_pages += nr_pages;
                /* No batch to abandon for a single folio -- the batch
                 * engine's goto move/out here is about OTHER folios
                 * already unmapped; there are none. -ENOMEM is still
                 * fatal to the caller's retry loop, though -- see
                 * migrate_folios() below. */
                return -ENOMEM;
        case -EAGAIN:
                return -EAGAIN;
        case 0:
                break;
        default:
                (*nr_failed)++;
                stats->nr_thp_failed += is_thp;
                stats->nr_failed_pages += nr_pages;
                return rc;
        }

        list_add_tail(&dst->lru, &dst_list);
        rc = migrate_folio_move(put_new_folio, private, folio, dst,
                        mode, reason, ret_folios);
        if (rc == -EAGAIN) {
                __migrate_folio_extract(dst, &old_folio_state, &anon_vma);
                list_del(&dst->lru);   /* required: migrate_folio_undo_dst()
                                         * does not unlink dst itself, and
                                         * migrate_folio_move()'s -EAGAIN path
                                         * re-links it into dst_list, which
                                         * is about to go out of scope. */
                migrate_folio_undo_src(folio, old_folio_state & FOLIO_WAS_MAPPED,
                                anon_vma, true, NULL);
                migrate_folio_undo_dst(dst, true, put_new_folio, private);
                return -EAGAIN;
        }
        if (rc) {
                (*nr_failed)++;
                stats->nr_thp_failed += is_thp;
                stats->nr_failed_pages += nr_pages;
        } else {
                stats->nr_succeeded += nr_pages;
                stats->nr_thp_succeeded += is_thp;
        }
        return rc;
}

static int migrate_folios(struct list_head *from, new_folio_t get_new_folio,
                free_folio_t put_new_folio, unsigned long private,
                enum migrate_mode mode, enum migrate_reason reason,
                struct list_head *ret_folios, struct list_head *split_folios,
                struct migrate_pages_stats *stats)
{
        /* folio_local_stats is this engine's own outward-facing scratch
         * (Part 3's pattern) -- distinct from pass_stats below, which is
         * an entirely separate, field-selective concern internal to the
         * async pre-pass. Every stats write in this function targets
         * folio_local_stats; it is merged into the caller-supplied
         * `stats` via gather_migrate_pages_stats() at each return point. */
        struct migrate_pages_stats folio_local_stats, pass_stats;
        LIST_HEAD(folios);
        int rc, nr_failed = 0;

        memset(&folio_local_stats, 0, sizeof(folio_local_stats));

        /* Real batching (deferred unmap-then-move across many folios) is
         * only safe in async mode (trylock-only, never blocks). Always
         * attempt one async batch pass: it's the whole job if the caller
         * wanted async, or a fast pre-check before falling back to
         * per-folio real-mode migration otherwise. nr_pass matches
         * mm/migrate.c:2138-2142 exactly: NR_MAX_MIGRATE_PAGES_RETRY for
         * pure async, NR_MAX_MIGRATE_ASYNC_RETRY for the pre-pass. */
        memset(&pass_stats, 0, sizeof(pass_stats));
        rc = migrate_folios_batch(from, get_new_folio, put_new_folio, private,
                        MIGRATE_ASYNC, reason, &folios, split_folios, &pass_stats,
                        mode == MIGRATE_ASYNC ? NR_MAX_MIGRATE_PAGES_RETRY
                                               : NR_MAX_MIGRATE_ASYNC_RETRY);
        folio_local_stats.nr_succeeded += pass_stats.nr_succeeded;
        folio_local_stats.nr_thp_succeeded += pass_stats.nr_thp_succeeded;
        folio_local_stats.nr_thp_split += pass_stats.nr_thp_split;
        folio_local_stats.nr_split += pass_stats.nr_split;

        if (mode == MIGRATE_ASYNC || rc < 0) {
                /* Final either way: nothing further will be attempted. */
                folio_local_stats.nr_failed_pages += pass_stats.nr_failed_pages;
                folio_local_stats.nr_thp_failed += pass_stats.nr_thp_failed;
                list_splice_tail(&folios, ret_folios);
                gather_migrate_pages_stats(stats, &folio_local_stats);
                return rc;
        }

        /* Non-split failures from the async pass -- permanent or
         * retry-exhausted alike -- get a full second chance below, so
         * they don't count yet (matches migrate_pages_sync()'s exact
         * comment and behavior, mm/migrate.c:2049-2053). THP splits are
         * already final -- a split folio isn't retried as that folio. */
        folio_local_stats.nr_thp_failed += pass_stats.nr_thp_split;
        nr_failed += pass_stats.nr_split;

        /* Stragglers: hugetlb-shaped -- this loop owns all retry state
         * directly, one full clean attempt per folio per pass. */
        list_splice_tail_init(&folios, from);
        {
                int retry = 1, nr_retry_pages = 0, thp_retry = 0, pass;
                struct folio *folio, *folio2;

                for (pass = 0; pass < NR_MAX_MIGRATE_SYNC_RETRY && retry; pass++) {
                        retry = 0;
                        nr_retry_pages = 0;
                        thp_retry = 0;

                        list_for_each_entry_safe(folio, folio2, from, lru) {
                                rc = migrate_folio_sync_one(folio, get_new_folio,
                                                put_new_folio, private, mode,
                                                reason, split_folios, ret_folios,
                                                &folio_local_stats, &nr_failed);
                                if (rc == -ENOMEM) {
                                        /* Fatal, matching today's
                                         * migrate_pages_sync() exactly: stop
                                         * immediately, leaving this folio and
                                         * everything not yet visited in
                                         * `from` for the caller's unconditional
                                         * splice. Fold in THIS pass's
                                         * already-accumulated -EAGAIN
                                         * stragglers as final failures now,
                                         * since they won't get the chance
                                         * they would have on a further pass
                                         * -- mirrors the batch engine's own
                                         * -ENOMEM abort, mm/migrate.c:1960-1962. */
                                        nr_failed += retry;
                                        folio_local_stats.nr_thp_failed += thp_retry;
                                        folio_local_stats.nr_failed_pages += nr_retry_pages;
                                        gather_migrate_pages_stats(stats, &folio_local_stats);
                                        return -ENOMEM;
                                }
                                if (rc == -EAGAIN) {
                                        retry++;
                                        /* folio is guaranteed unsplit and
                                         * otherwise untouched on -EAGAIN, so
                                         * its own fields are still accurate. */
                                        thp_retry += folio_test_pmd_mappable(folio);
                                        nr_retry_pages += folio_nr_pages(folio);
                                }
                        }
                }
                nr_failed += retry;
                folio_local_stats.nr_thp_failed += thp_retry;
                folio_local_stats.nr_failed_pages += nr_retry_pages;

                gather_migrate_pages_stats(stats, &folio_local_stats);
                return nr_failed;
        }
}
```

`migrate_folios_unmap()` (Part 1) is updated in the same milestone to call `migrate_folio_precheck()` too, replacing its inline copy of these same three checks — see Dependencies and Sequence for why this doesn't reopen Part 1's already-converged AC-1/AC-2.

`migrate_folios()`'s use of `gather_migrate_pages_stats(stats, &folio_local_stats)` at each of its own return points (rather than a per-field manual merge) is valid and safe specifically because `folio_local_stats` is the product of exactly one self-contained call to `migrate_folios()` — a wholesale merge into the caller's `stats` is correct. This is a distinct concern from the async pre-pass's field-selective merge above (`pass_stats` into `folio_local_stats`, not the generic `gather_migrate_pages_stats()`), which must remain field-selective to avoid double-counting (see Claude-Codex Deliberation).

### `migrate_pages()` top level

```c
int migrate_pages(struct list_head *from, new_folio_t get_new_folio,
                free_folio_t put_new_folio, unsigned long private,
                enum migrate_mode mode, enum migrate_reason reason,
                unsigned int *ret_succeeded)
{
        int rc, rc_gather;
        int nr_pages;
        struct folio *folio, *folio2;
        LIST_HEAD(folios);
        LIST_HEAD(ret_folios);
        LIST_HEAD(split_folios);
        struct migrate_pages_stats stats;              /* grand total --
                                                          * the ONLY stats
                                                          * struct this
                                                          * function ever
                                                          * declares. */

        trace_mm_migrate_pages_start(mode, reason);
        memset(&stats, 0, sizeof(stats));

        /* Each engine below owns its own local stats struct internally
         * and merges into &stats at its own return point (Part 3) -- no
         * per-engine local or gather_migrate_pages_stats() call belongs
         * here; &stats is simply threaded straight through. */
        rc_gather = migrate_hugetlbs(from, get_new_folio, put_new_folio, private,
                        mode, reason, &stats, &ret_folios);
        if (rc_gather < 0)
                goto out;

        rc = migrate_movable_ops_pages(from, get_new_folio, put_new_folio, private,
                        mode, reason, &stats, &ret_folios);
        if (rc < 0) {
                rc_gather = rc;
                goto out;
        }
        rc_gather += rc;

again:
        nr_pages = 0;
        list_for_each_entry_safe(folio, folio2, from, lru) {
                /* Stragglers that exhausted migrate_hugetlbs()'s or
                 * migrate_movable_ops_pages()'s own retry budget -- give
                 * up on them here rather than letting them reach the
                 * folio engine. */
                if (folio_test_hugetlb(folio) || page_has_movable_ops(&folio->page)) {
                        list_move_tail(&folio->lru, &ret_folios);
                        continue;
                }
                nr_pages += folio_nr_pages(folio);
                if (nr_pages >= NR_MAX_BATCHED_MIGRATION)
                        break;
        }
        if (nr_pages >= NR_MAX_BATCHED_MIGRATION)
                list_cut_before(&folios, from, &folio2->lru);
        else
                list_splice_init(from, &folios);

        rc = migrate_folios(&folios, get_new_folio, put_new_folio,
                        private, mode, reason, &ret_folios,
                        &split_folios, &stats);
        list_splice_tail_init(&folios, &ret_folios);
        if (rc < 0) {
                rc_gather = rc;
                list_splice_tail(&split_folios, &ret_folios);
                goto out;
        }
        if (!list_empty(&split_folios)) {
                /* One-shot, single-pass cleanup call -- not a
                 * self-contained "engine" with its own retry loop, so it
                 * writes into &stats directly (purely additive, exactly
                 * like every other write in this function; no local
                 * needed for a single call). */
                migrate_folios_batch(&split_folios, get_new_folio,
                                put_new_folio, private, MIGRATE_ASYNC, reason,
                                &ret_folios, NULL, &stats, 1);
                list_splice_tail_init(&split_folios, &ret_folios);
        }
        rc_gather += rc;
        if (!list_empty(from))
                goto again;
out:
        list_splice(&ret_folios, from);
        if (list_empty(from))
                rc_gather = 0;

        count_vm_events(PGMIGRATE_SUCCESS, stats.nr_succeeded);
        count_vm_events(PGMIGRATE_FAIL, stats.nr_failed_pages);
        count_vm_events(THP_MIGRATION_SUCCESS, stats.nr_thp_succeeded);
        count_vm_events(THP_MIGRATION_FAIL, stats.nr_thp_failed);
        count_vm_events(THP_MIGRATION_SPLIT, stats.nr_thp_split);
        trace_mm_migrate_pages(stats.nr_succeeded, stats.nr_failed_pages,
                       stats.nr_thp_succeeded, stats.nr_thp_failed,
                       stats.nr_thp_split, stats.nr_split, mode, reason);

        if (ret_succeeded)
                *ret_succeeded = stats.nr_succeeded;

        return rc_gather;
}
```

### Relevant References

- `mm/migrate.c:1727` `migrate_folios_move()`, `:1779` `migrate_folios_undo()` — naming/parameter precedent Part 1 mirrors.
- `mm/migrate.c:1811-2021` — the batch engine being decomposed (Part 1) and renamed (Part 4).
- `mm/migrate.c:1964-1967` / `:1994-1996` — the `try_to_unmap_flush()` conditionality Part 1 must preserve exactly (verified directly by reading the current source).
- `mm/migrate.c:1638-1725` `migrate_hugetlbs()` / `:1480-1589` `unmap_and_move_hugetlb_folio()` — the structural precedent Parts 2 and 4's retry loops both mirror.
- `mm/migrate.c:2116` `migrate_hugetlbs()` call site in `migrate_pages()`, `:2121-2137` the `again:`/`NR_MAX_BATCHED_MIGRATION` batch-splitting loop including its `folio_test_hugetlb()` safety-net check — the precedent Part 2's dispatch mirrors.
- `mm/migrate.c:237` `migrate_movable_ops_page()` — existing driver-dispatch helper Part 2 reuses. `mm/migrate.c:55-297` — the full existing `movable_ops` helper block, natural home for Part 2's new functions. `mm/migrate.c:230-233` — the David Hildenbrand TODO motivating Part 2 (verified via `git blame`, commit `b9ed00483d4cba`, 2025-07-04).
- `mm/migrate.c:1213` `migrate_folio_unmap()`, `:1363` `migrate_folio_move()` — the existing single-folio primitives Part 4's `migrate_folio_sync_one()` composes, reusing their existing `__migrate_folio_record`(`:1156`)/`__migrate_folio_extract`(`:1162`) state handoff via `dst->migrate_info`, and their existing `migrate_folio_undo_src`(`:1173`)/`migrate_folio_undo_dst`(`:1188`) cleanup helpers (reused directly by `migrate_folio_sync_one()`'s `-EAGAIN` unwind).
- `mm/migrate.c:877` `__migrate_folio()`'s `folio_ref_count(src) != expected_count` check — the root cause of the move-step `-EAGAIN` that motivates `migrate_folio_sync_one()`'s design.
- `mm/migrate.c:1448-1451` `migrate_folio_move()`'s `-EAGAIN` path (leaves `src`/`dst` locked, `dst` re-queued, expects in-place retry) — the batching-era contract `migrate_folio_sync_one()` must not naively inherit.
- Commit `8a27e6b478415` "migrate_pages: restrict number of pages to migrate in batch" — the commit that introduced `migrate_pages_batch()`/the whole batch engine. Its **parent** commit's `mm/migrate.c` (fetched via `git show 8a27e6b478415^:mm/migrate.c`) contains the pre-batching `__unmap_and_move()`/`unmap_and_move()`, directly verified to unconditionally unlock and release `dst` on any non-success outcome including the identical refcount-race `-EAGAIN`, and to never use deferred TLB flushing — the historical precedent validating `migrate_folio_sync_one()`'s unwind-on-`-EAGAIN` design.
- `mm/migrate.c:2023-2072` `migrate_pages_sync()` (current) — what Part 4 replaces; its shadow-`astats`-merge logic (mm/migrate.c:2038-2053, including the comment at :2049-2051) is what Part 4's `migrate_folios()` field-selective merge reproduces exactly (not the generic `gather_migrate_pages_stats()` helper, which is reserved for the always-safe wholesale merges in Part 3).
- `mm/migrate.c:1620` `struct migrate_pages_stats` — the struct Part 3 splits into per-engine instances.
- `mm/migrate.c:1838-1917` — the pre-unmap fast paths (deferred-split, unsupported-THP split, `folio_ref_count(folio) == 1`) extracted into `migrate_folio_precheck()`, shared by `migrate_folios_batch()`'s loop and `migrate_folio_sync_one()`; `mm/migrate.c:1929-1967` — the post-unmap `-ENOMEM` handling (large-folio split retry, `MR_LONGTERM_PIN` special case), whose batch-abandon (`goto move`/`goto out`) portion has no meaning for a single folio and is *not* shared verbatim — `migrate_folio_sync_one()` implements the correct degenerate singleton form directly.
- Precedent commits: `64c8902ed4418` (Huang Ying, single-folio unmap/move split — the split that introduced the batching-era in-place-retry contract `migrate_folio_sync_one()` works around for the unbatched case), `f752e677f8599` (Byungchul Park, Reviewed-by: Zi Yan, batch move/undo split).
- `scripts/checkpatch.pl`, `tools/testing/selftests/mm/migration.c`/`ksft_migration.sh`, `mm/Makefile:98` — verification tooling, as established for Part 1.

## Dependencies and Sequence

### Milestones

1. **Extract `migrate_folios_unmap()`** (Part 1). Prerequisite for the rest — not a hard technical blocker, but it shrinks Milestone 2's diff surface (editing an isolated helper instead of the original 200-line inline loop) and is already independently Codex-converged.
   - Phase A: Enumerate every value the unmap loop produces that the batch engine consumes afterward.
   - Phase B: Define the helper's signature/return contract, unambiguously distinguishing the move/out outcome.
   - Phase C: Move the loop body verbatim; wire up the call site, preserving the conditional flush.
2. **Isolate `movable_ops` pages** (Part 2). Depends on Milestone 1 for a smaller diff, not for correctness.
   - Phase A: Write `migrate_one_movable_ops_page()` (self-contained: dispatch, cleanup, and its own list placement) and `migrate_movable_ops_pages()`, mirroring `unmap_and_move_hugetlb_folio()`/`migrate_hugetlbs()` exactly, including the `PF_MEMALLOC`/`MIGRATE_SYNC_LIGHT` lock-acquisition rules and the on-success `dst` reference drop.
   - Phase B: Wire into `migrate_pages()` before the batch-splitting loop, alongside `migrate_hugetlbs()`; add the `again:`-loop safety-net check.
   - Phase C: Remove the three now-dead `page_has_movable_ops()` branches from `migrate_folio_unmap()`, `migrate_folio_move()`, and the folio engine's fast-path check — `migrate_folio_done()`'s guard stays, now serving two callers.
3. **Split stats per engine and restructure the folio engine's retry policy** (Parts 3 and 4 together — they're two views of the same change: Part 3's internal-local-plus-merge-at-return pattern is what lets `migrate_pages()` thread a single `&stats` straight through every engine call with no per-engine local of its own, while `migrate_folios()`'s async-pre-pass portion still needs its own, separate local `pass_stats` and a field-selective merge, matching `migrate_pages_sync()`'s original accounting exactly). Depends on Milestone 2 for `migrate_movable_ops_pages()` to exist at all (nothing to give this pattern to before that function exists).
   - Phase A: Add `gather_migrate_pages_stats()`. Give `migrate_movable_ops_pages()` (Part 2) and `migrate_hugetlbs()` (existing, otherwise unmodified) each an internal local stats struct and a `gather_migrate_pages_stats()` call at every one of their own return points, taking the caller's `stats` pointer directly as a parameter — no locals or merge calls remain in `migrate_pages()` itself for these two.
   - Phase B: Rename `migrate_pages_batch()` → `migrate_folios_batch()` (pure rename).
   - Phase C: Extract `migrate_folio_precheck()` from `migrate_folios_unmap()`'s inline pre-unmap checks (Part 1); update `migrate_folios_unmap()` to call it instead of its inline copy. Small internal change to Part 1's already-converged function, not a reopening of AC-1/AC-2's external-behavior guarantees.
   - Phase D: Write `migrate_folio_sync_one()`, calling `migrate_folio_precheck()` plus `migrate_folio_unmap()` once and `migrate_folio_move()` at most once, with a correctly-scoped singleton `-ENOMEM` handler and an explicit unwind (including `list_del(&dst->lru)`) of a move-step `-EAGAIN`; restructure `migrate_pages_sync()` into `migrate_folios()` with the unified call site, an internal `folio_local_stats` (accumulating both the field-selective async-pre-pass merge and the hugetlb-shaped fallback loop's direct writes) merged into the caller's `stats` via `gather_migrate_pages_stats()` at each of `migrate_folios()`'s own return points.
   - Phase E: Wire `migrate_pages()`'s `again:`-loop call to `migrate_folios()` (and the `split_folios` follow-up `migrate_folios_batch()` call) to pass `&stats` directly — no per-iteration local, no explicit merge call at the `migrate_pages()` call site.
4. **Verify no functional change** (outside the accepted exception family documented in AC-7/AC-7.1), applied across all four parts: `scripts/checkpatch.pl --strict`; build `mm/migrate.o`; best-effort selftest run; independent (Codex) review per AC-2.1, AC-5.1, and AC-7.1.

## Task Breakdown

| Task ID | Description | Target AC | Tag (`coding`/`analyze`) | Depends On |
|---------|-------------|-----------|----------------------------|------------|
| task1 | Extract the unmap-phase loop into `migrate_folios_unmap()`, preserving exact control flow and the conditional `try_to_unmap_flush()` behavior | AC-1, AC-2 | coding | - |
| task2 | Write commit message(s) documenting each part's rationale (one per part, per AC-4); ensure in-code comments describe only current structure | AC-4 | coding | task1, task5, task11 |
| task3 | Write `migrate_one_movable_ops_page()` (self-contained: dispatch, lock-acquisition parity with today's shared code, on-success `dst` reference drop, own list placement) and `migrate_movable_ops_pages()` (owning its own internal local stats struct, merged into the caller's `stats` at each return point per AC-6), mirroring `unmap_and_move_hugetlb_folio()`/`migrate_hugetlbs()`'s structure including the retry loop | AC-5, AC-6 | coding | task1 |
| task4 | Wire `migrate_movable_ops_pages()` into `migrate_pages()` before the batch-splitting loop, alongside `migrate_hugetlbs()`; add the `again:`-loop safety-net check | AC-5 | coding | task3 |
| task5 | Remove the three now-dead `page_has_movable_ops()` branches from `migrate_folio_unmap()`, `migrate_folio_move()`, and the folio engine's fast-path check — leave `migrate_folio_done()`'s guard in place | AC-5 | coding | task4 |
| task6 | Add `gather_migrate_pages_stats()`; give `migrate_hugetlbs()` (existing, otherwise unmodified) an internal local stats struct and a merge-at-return call at each of its own return points, taking the caller's `stats` pointer directly; remove any per-engine local/merge call from `migrate_pages()` itself (`migrate_movable_ops_pages()`'s own local was already added in task3) | AC-6 | coding | task4 |
| task7 | Rename `migrate_pages_batch()` to `migrate_folios_batch()` (pure rename, no behavior change) | AC-7 | coding | task5 |
| task8 | Extract `migrate_folio_precheck()` (`bool` return) from `migrate_folios_unmap()`'s inline pre-unmap checks; update `migrate_folios_unmap()` to call it | AC-7 | coding | task7 |
| task9 | Write `migrate_folio_sync_one()`: call `migrate_folio_precheck()`, then `migrate_folio_unmap()` once and `migrate_folio_move()` at most once; implement the singleton-scoped `-ENOMEM` handler including the `MR_NUMA_MISPLACED` `nosplit` guard; explicitly unwind a move-step `-EAGAIN` via `__migrate_folio_extract()` + `list_del(&dst->lru)` + the existing `migrate_folio_undo_src()`/`migrate_folio_undo_dst()` helpers | AC-7 | coding | task8 |
| task10 | Restructure `migrate_pages_sync()` into `migrate_folios()`: unified async-pass call site (correct mode-dependent `nr_pass`), an internal `folio_local_stats` receiving the field-selective async-pre-pass merge matching the original exactly plus the hugetlb-shaped fallback loop's direct writes, merged into the caller's `stats` via `gather_migrate_pages_stats()` at each of `migrate_folios()`'s own return points, hugetlb-shaped per-folio fallback loop calling `migrate_folio_sync_one()` with no list operations or independent stats interpretation in its own switch, and `-ENOMEM` treated as fatal with same-pass `-EAGAIN` stragglers folded into final counts before returning | AC-6, AC-7 | coding | task9 |
| task11 | Wire `migrate_pages()`'s `again:`-loop call to `migrate_folios()` (and the `split_folios` follow-up `migrate_folios_batch()` call) to pass `&stats` directly — no per-iteration local, no explicit merge call at the call site | AC-6, AC-7 | coding | task6, task10 |
| task12 | Run `scripts/checkpatch.pl --strict` against all commits; fix ERROR-level findings, address/justify WARNING/CHECK-level ones | AC-3 | coding | task2, task5, task11 |
| task13 | Build `mm/migrate.o` under the current `.config`; confirm no new warnings (hard gate) | AC-2, AC-5, AC-7 | coding | task12 |
| task14 | Attempt `tools/testing/selftests/mm/migration.c` + `ksft_migration.sh` before/after; record results or why not | AC-2 | coding | task13 |
| task15 | Independent review of `migrate_folios_unmap()` for line-by-line semantic equivalence, specifically the `try_to_unmap_flush()` conditionality | AC-2.1 | analyze | task1, task2 |
| task16 | Independent review of `migrate_movable_ops_pages()` for isolate/putback semantic equivalence, specifically `PageMovableOpsIsolated` transitions, the accepted mode-sequencing/fresh-`dst`-per-retry simplifications, and failure-path cleanup | AC-5.1 | analyze | task5 |
| task17 | Independent review of `migrate_folios()`'s and `migrate_folio_sync_one()`'s accounting against today's `migrate_pages_batch()`/`migrate_pages_sync()`, per AC-7.1(a)-(h): the split-THP charge, double-counting risk, the `-ENOMEM` early-return path, `migrate_folio_precheck()` equivalence, the singleton `-ENOMEM` handler's correctness as a degenerate case, the same-pass/cross-pass `-ENOMEM` fold-in accounting (g), and the accepted move-step `-EAGAIN` retry-economy divergence (h) | AC-7.1 | analyze | task11 |

## Claude-Codex Deliberation

### Agreements (Part 1 — from the original narrower plan's convergence)

- The extraction correctly mirrors existing local precedent (`migrate_folios_move()`/`migrate_folios_undo()`) and the two cited precedent commits.
- AC-1/AC-2 correctly identified the risky control-flow areas: `-ENOMEM`, `-EAGAIN`, THP/large-folio split handling, deferred-split handling, retry accounting, list movement.
- An independent (Codex) semantic-equivalence review is a useful acceptance item for a no-functional-change refactor.

### Resolved Disagreements (Part 1 — 3 rounds, converged)

- **`try_to_unmap_flush()` call timing**: an earlier draft's claim ("called exactly once, after the unmap phase, before the move phase") was factually wrong — verified directly against mm/migrate.c:1964-1996: the `-ENOMEM`/empty-`unmap_folios` path bypasses the `move:` label entirely. Fixed: AC-2 now states the exact conditional behavior.
- **`struct migrate_control` as an in-plan "upper bound"**: too large a scope addition for the original narrower plan. Deferred to DEC-1, unaffected by this redesign.
- **Line-count/complexity as an acceptance metric**: too mechanical; the real bar is a clean helper boundary with unchanged semantics. AC-1 frames it as supporting evidence only.
- **`ksft_migration.sh` as a hard gate**: impractical depending on environment. AC-2 treats it as best-effort.
- **AC-4's code-comment wording**: kernel comments shouldn't advertise unmerged future patches. AC-4 separates commit-message rationale from code-comment constraints.
- **checkpatch acceptance wording**: exit-status alone conflates errors and warnings. AC-3 distinguishes ERROR from WARNING/CHECK.
- **AC-1's grep test hard-coded `static int`**, contradicting the enum/result-struct option. Fixed to match on presence and outcome-contract clarity, not a specific return type.

### Design corrections found before Codex review (direct discussion)

Two real bugs were found and fixed before the first Codex pass on Parts 2-4, in the same spirit as the Part 1 corrections above:

- **A naive `migrate_movable_ops_pages()` sketch was single-shot (no retry loop)**. Corrected after establishing that `movable_ops` pages currently get real retry benefit today via shared `folio_trylock()` contention handling in the batch engine's own pass loop — dropping that would have been a silent regression. Fixed by structuring it exactly like `migrate_hugetlbs()`.
- **A naive "call `migrate_folios_batch()` with `nr_pass=1` repeatedly from an outer loop" approach to unifying Part 4's retry shape would double-count folios.** Traced precisely against mm/migrate.c:1968-1993: the low-level batch engine's own "exhausted retries" accounting fires whenever *its* internal pass budget runs out, which with `nr_pass=1` means every single `-EAGAIN` gets prematurely charged as failed, even though the folio remains in the list for the outer loop to retry — and if it later succeeds, it would be double-counted.

### Codex Round 1 on Parts 2-4 (findings and verification)

A first Codex convergence pass was run specifically against Parts 2-4 (full transcript covered AC-5 through AC-7.1). It found a substantial list of real issues, all independently re-verified against the actual source before being accepted — none were taken on Codex's word alone, matching the same rigor applied to Part 1's review:

- **`migrate_folio_sync_one()`'s naive unmap+move composition was fundamentally broken.** Codex flagged that `migrate_folio_move()` assumes `dst->lru` is already list-linked (`prev = dst->lru.prev; list_del(&dst->lru);` unconditionally at its top) and that its `-EAGAIN` path leaves `src`/`dst` locked with no unlock, expecting an in-place retry. Verified directly by reading `migrate_folio_move()` in full (mm/migrate.c:1363-1459): both claims confirmed exactly. A naive one-shot composition would either operate on an uninitialized list node or leak locks/deadlock on retry. **Resolved** by researching the pre-batching `__unmap_and_move()` (see below) rather than guessing at a fix, which led to the final `migrate_folio_sync_one()` design in Part 4 above.
- **The `NR_MAX_BATCHED_MIGRATION` constant was wrong for the async-mode `nr_pass` budget.** Codex flagged that `mode == MIGRATE_ASYNC` should use `NR_MAX_MIGRATE_PAGES_RETRY`, not `NR_MAX_BATCHED_MIGRATION` (a batch-*size* threshold, ~512, not a retry-pass count). Verified directly against mm/migrate.c:2138-2142: confirmed, the current code uses `NR_MAX_MIGRATE_PAGES_RETRY` (10). This was a real memory error on my part, not a legitimate design choice — fixed in AC-7 and the Part 4 code above.
- **The AC-7 wholesale `pass_stats` merge would double-count.** Codex flagged that the original `migrate_pages_sync()` intentionally discards the async pre-pass's `nr_failed_pages`/`nr_thp_failed` on the non-error, non-async path (they get a real second chance below) and only carries `nr_succeeded`/`nr_thp_succeeded`/`nr_split`/`nr_thp_split` plus `nr_thp_failed += nr_thp_split`. Verified directly against mm/migrate.c:2038-2053, including the exact comment explaining why. My "unify the call site" simplification had merged all six fields unconditionally, silently reintroducing the double-counting bug I'd already fixed once. **Resolved** by reverting to the field-selective merge (see Part 4 code above) while keeping the call-site unification (which was independently correct and not the source of the bug).
- **Part 2's `migrate_folio_done()` guard removal was not behavior-preserving.** Codex flagged that removing the `page_has_movable_ops()` guard from `migrate_folio_done()` (mm/migrate.c:1203) while still calling it for movable pages would wrongly decrement `NR_ISOLATED_ANON`/`FILE` for non-LRU pages that were never counted there. Verified directly against `isolate_folio_to_list()` (mm/migrate.c:280-297): confirmed, only the non-movable_ops branch does that accounting at isolation time. **Resolved**: the guard stays; AC-5 corrected to remove only 3 of the 4 branches.
- **Part 2's success path was missing real cleanup.** Codex flagged that the original `migrate_one_movable_ops_page()` sketch didn't drop `dst`'s reference or set the page-owner migrate reason on success. Verified directly against `migrate_folio_move()`'s `out_unlock_both:` block (mm/migrate.c:1421-1429): confirmed, `folio_set_owner_migrate_reason(dst, reason)` and `folio_put(dst)` are both required and were missing — a real reference leak. **Resolved** in the corrected Part 2 code above, which also now handles its own list placement (`list_del`/`list_move_tail`), matching `unmap_and_move_hugetlb_folio()`'s actual pattern rather than leaving it to the outer loop.
- **Part 2 was also missing lock-acquisition parity.** Codex flagged that the `PF_MEMALLOC` and `MIGRATE_SYNC_LIGHT && !folio_test_uptodate(src)` non-blocking-exit checks — which today's shared code already applies to `movable_ops` pages before reaching their branch — were dropped in the simplified sketch. Verified directly against mm/migrate.c:1249-1258 (runs before the `page_has_movable_ops()` check at :1312): confirmed. **Resolved** in the corrected Part 2 code above.

### Historical verification: why `migrate_folio_sync_one()`'s `-EAGAIN` design is correct, not merely convenient

The repository owner asked a pointed question that reframed the `migrate_folio_move()` `-EAGAIN` problem: before `migrate_pages_batch()` existed, did unmap+move work as a single retried-from-scratch operation? Verified by reading the actual pre-batching source: the parent of commit `8a27e6b478415` ("migrate_pages: restrict number of pages to migrate in batch" — the commit that introduced `migrate_pages_batch()` and the whole batch-engine split in the first place) shows `__unmap_and_move()` handling the *identical* refcount-race `-EAGAIN` (from what is now `__migrate_folio()`'s `folio_ref_count(src) != expected_count` check, mm/migrate.c:877) by unconditionally unlocking both folios and releasing `dst` — no in-place-retry state, ever — and never using deferred/batched TLB flushing at all (`try_to_migrate(src, 0)`, no `TTU_BATCH_FLUSH`). This was the real, shipped, correct behavior for exactly this scenario (single folio, real sync mode) for years before batching existed. `migrate_folio_move()`'s current in-place-retry contract is a batching-specific optimization introduced when `unmap_and_move()` was split (commit `64c8902ed4418`) specifically to avoid redundant work across *many* folios sharing one deferred flush — a benefit that doesn't apply to a path that only ever processes one folio at a time. This is why `migrate_folio_sync_one()`'s explicit unwind-on-`-EAGAIN` (rather than a bounded inner retry loop, which was an earlier, more complicated candidate fix) is the historically-grounded, not just pragmatically-convenient, design.

### Codex Round 2 on Parts 2-4 (findings and verification)

A second Codex round reviewed the round-1-corrected plan, specifically the historically-grounded `migrate_folio_sync_one()`. It found the `-EAGAIN`-unwind design sound in principle (explicitly agreed) but caught real implementation gaps:

- **Missing `list_del(&dst->lru)` before `migrate_folio_undo_dst()` in the `-EAGAIN` unwind.** `migrate_folio_move()`'s own `-EAGAIN` path re-links `dst->lru` via `list_add()`; `migrate_folio_undo_dst()` only unlocks/releases the folio, it does not unlink it. Without an explicit `list_del()` first, `dst` stays linked into a stack-local list that's about to go out of scope. **Resolved**: added directly in the code above.
- **The composition silently skipped the batch engine's pre-unmap fast paths and post-unmap `-ENOMEM` handling** (deferred-split, unsupported-THP-split, refcount==1 fast path, large-folio-split-with-`MR_LONGTERM_PIN`-retry) and the associated THP counters. Verified directly against mm/migrate.c:1838-1993: confirmed, all of this lives outside `migrate_folio_unmap()`/`migrate_folio_move()`, inline in the batch loop, and `migrate_folio_sync_one()` had never accounted for it. **Resolved**: rather than duplicating this logic (the repository owner's suggestion, judged better than either duplicating it blind or abandoning the hugetlb-shaped design entirely), the pre-unmap checks were extracted into a new shared `migrate_folio_precheck()` (`bool` return, per the repository owner's simplification of an earlier two-state enum draft), called by both `migrate_folios_batch()`'s loop and `migrate_folio_sync_one()` — see the Part 4 code above and its accompanying rationale. The post-unmap `-ENOMEM` handling was *not* shared verbatim, since its batch-abandon (`goto move`/`goto out`) portion has no meaning for a single folio (for a true singleton, `unmap_folios` is necessarily empty when `-ENOMEM` occurs, so the existing logic already degenerates to "clean up and return" in that case) — `migrate_folio_sync_one()` implements the correct simpler singleton form directly.
- **`migrate_movable_ops_pages()` changes the non-async mode sequence for `movable_ops` pages.** Today, because these pages aren't yet separated from the regular-folio engine, they incidentally inherit `migrate_pages_sync()`'s async-pre-pass-then-real-mode-fallback policy; the new pass uses the caller's real mode from the first attempt. Flagged by Codex as a real behavior change (different mode sequence visible to drivers, source locking can block earlier) and left `UNRESOLVED` pending an explicit decision. **Resolved by the repository owner**: accepted as an intentional simplification, not a gap to fix — `movable_ops` pages are primarily VM balloon/zsmalloc/offline pages, a use case that doesn't need the async-fast-path optimization built for bulk regular-folio migration throughput. AC-5.1 updated to state this exception explicitly (along with the related, same-rationale acceptance that `migrate_one_movable_ops_page()` allocates a fresh `dst` per outer-pass retry rather than reusing one across a driver-level `-EAGAIN`, also flagged by Codex, also accepted on the same grounds).
- Also noted by Codex: the long historical rationale should live in commit messages, not code comments (consistent with AC-4, already a stated constraint) — the code comments in the Part 4 sketch above are intentionally short, pointing at the *current* lock/list contract rather than re-narrating the archaeology.

### Codex Round 3 on Parts 2-4 (findings and verification)

A third Codex round reviewed the `migrate_folio_precheck()` extraction and the singleton `-ENOMEM` handler specifically. It agreed the precheck extraction correctly reproduces the three pre-unmap paths (including their stats/list side effects) and that the `-EAGAIN` unwind is now conceptually correct, but found real remaining gaps, all confirmed:

- **`-ENOMEM` from `migrate_folio_sync_one()` was not treated as fatal by the fallback loop, and the folio was neither moved to `ret_folios` nor removed from further consideration.** The loop just continued to the next folio — a real deviation from today's `migrate_pages_sync()`, which returns `-ENOMEM` immediately, and a real double-counting risk (the folio could be silently revisited on a later pass). **Resolved**: the fallback loop now treats `migrate_folio_sync_one()` returning `-ENOMEM` as fatal — see the corrected code above.
- **The current pass's already-accumulated `-EAGAIN` stragglers need folding into final failure counts when a later folio in the same pass hits `-ENOMEM`** — this scenario is only possible because of the new shared-pass loop shape (today's per-folio `migrate_pages_batch()` calls have no equivalent, since each folio gets its own independent budget). Mirrors the batch engine's own `-ENOMEM`-abort fold-in (mm/migrate.c:1960-1962) exactly. **Resolved**: added directly in the fold-in-then-return code above; this is also the scenario Codex asked to see traced explicitly in AC-7.1(g).
- **Missing the `MR_NUMA_MISPLACED` → `nosplit` guard** in the singleton `-ENOMEM` handler (mm/migrate.c:1828 has it; the sketch didn't). **Resolved**: added.
- **`cond_resched()` instead of `cond_resched_tasks_rcu_qs()`** in the fallback loop, dropping an RCU-tasks quiescent-state side effect the batch engine has (mm/migrate.c:1843). **Resolved**: moved `cond_resched_tasks_rcu_qs()` into `migrate_folio_precheck()` itself, so both callers get it from one place instead of each needing to remember it.
- **A real compile-level type bug**: `migrate_folio_precheck()` takes `bool *is_thp`, but `migrate_folio_sync_one()` had declared `is_thp` as `int`. **Resolved**: declared `bool` alongside `is_large`/`nosplit`.

**Not yet re-verified by a further Codex round** against this latest-corrected version. See AC-7.1's seven-point review scope, specifically point (g) — the `-EAGAIN`-then-`-ENOMEM`-same-pass interaction Codex asked to see traced.

### Codex Round 4 on Parts 2-4 (findings and verification)

A fourth Codex round specifically traced the AC-7.1(g) scenario round 3 asked to see verified (folio A returns `-EAGAIN`, folio B later in the same pass returns `-ENOMEM`), checked whether leaving the `-ENOMEM`-triggering folio and unvisited folios untouched on `from` is genuinely correct given `migrate_pages()`'s unconditional `list_splice_tail_init()` after the call, and checked whether moving `cond_resched_tasks_rcu_qs()` into `migrate_folio_precheck()` changed its position relative to anything meaningful. It agreed all three were now correct, but surfaced a deeper structural point not raised by rounds 1-3:

- **Accounting correctness (agreed, no fix needed).** Folio A's `-EAGAIN` is counted exactly once via the fold-in (`nr_retry_pages`/`thp_retry`), and folio B's `-ENOMEM` accounting is already finalized inside `migrate_folio_sync_one()` before it returns — the outer fold-in does not double-count B, and A is not lost. Leaving the triggering folio and not-yet-visited folios untouched on `from` was independently confirmed correct for the one planned caller: `migrate_pages()` does an unconditional `list_splice_tail_init(&folios, &ret_folios)` immediately after `migrate_folios()` returns, regardless of return value, matching where today's `migrate_pages_sync()` path's survivors end up externally. Moving `cond_resched_tasks_rcu_qs()` into `migrate_folio_precheck()` was confirmed to preserve its position relative to lock acquisition and to the `is_thp`/`nr_pages` snapshots — no meaningful reordering.
- **Deeper finding: the shared-pass loop shape is not equivalent to today's retry *opportunity*, independent of the accounting being locally correct.** Today, `migrate_pages_sync()`'s folio-major loop gives folio A its *entire* `NR_MAX_MIGRATE_SYNC_RETRY`-attempt budget before ever touching folio B. In the new pass-major (`migrate_hugetlbs()`-shaped) loop, B can hit `-ENOMEM` in pass 0 while A has had only one attempt; folding A into final failure at that point can change `nr_succeeded`/`nr_failed_pages`/THP stats/`ret_folios` membership versus today, in the case where A would have succeeded on a later singleton retry it never gets to take. This is a genuine behavioral divergence, not an accounting bug — confirmed by re-reading `migrate_pages_sync()`'s `while (!list_empty(from))` structure (mm/migrate.c:2023-2072) against the new `for (pass...) { list_for_each_entry_safe(...) }` structure and verifying no equivalent early-abort-with-fold-in exists in the old code, because the old code never processes more than one folio per "logical pass" of a folio's own retry loop.
- Required change identified: either restore per-folio-to-exhaustion retry semantics for the sync fallback, or explicitly document the round-robin ordering as an accepted behavior change and relax the "bit-identical" claim — plus reword AC-7.1(g) to distinguish "counted once under the new loop" (which holds) from "equivalent retry opportunity to today" (which does not).
- **Resolved by the repository owner**: accept the round-robin retry-ordering as an intentional, documented behavior change rather than restoring the old per-folio-to-exhaustion shape. Rationale: the round-robin shape is what makes `migrate_folios()` structurally consistent with `migrate_hugetlbs()` (which already uses this exact pass-major/folio-minor pattern today) and with the rest of this redesign's stated goal of a uniform retry-loop shape across engines; the divergence is narrow (only triggers when `-ENOMEM` is reached by any folio while another folio in `from` has not yet had its own outcome finalized) and `-ENOMEM` is itself an abnormal error path (destination allocation failure under `get_new_folio()`, not necessarily whole-system memory exhaustion), not a normal-operation code path where subtle statistics differences would be noticed. AC-7 and AC-7.1(g) have been reworded accordingly (see above and Round 5 below) rather than carrying this as an open `UNRESOLVED`/`DEC-N` item, since the decision itself is final — only the documentation needed updating.

### Codex Round 5 on Parts 2-4 (findings and verification)

A fifth Codex round reviewed the round-4 documentation update (AC-7's exception paragraph, the "bit-identical...except" carve-out, AC-7.1(g)'s reworded scope, and the new Round-4 Deliberation entry). It agreed AC-7.1(g)'s new wording successfully avoids claiming retry-opportunity equivalence while remaining a real, checkable accounting point, and agreed the `migrate_hugetlbs()`-consistency rationale is accurate — but found the documented exception itself was too narrow, plus wording and consistency gaps elsewhere in the plan, all independently verified before being accepted:

- **The exception as first documented covered only the same-pass `-EAGAIN`-then-`-ENOMEM` fold-in case, missing a broader, genuinely distinct cross-pass case.** Codex's concrete counter-example: list order `[B, A]` (B before A); new round-robin loop's pass 0 attempts B (`-EAGAIN`) then A (succeeds, `rc = 0`); pass 1 attempts B again, which now returns `-ENOMEM`. Final new-loop stats count A as succeeded. Verified directly against `migrate_pages_sync()`'s actual structure (mm/migrate.c:2059-2069, re-read in full for this round): it pops exactly one folio at a time via `list_move(from->next, &folios)` into a private singleton list and drives *only that folio* through its own full `NR_MAX_MIGRATE_SYNC_RETRY`-attempt `migrate_pages_batch()` call before the next folio in `from` is even looked at — so under today's design, B (first in list) would still be mid-retry (or would have just returned `-ENOMEM` from its own singleton call, causing an immediate `return rc` at mm/migrate.c:2066-2067) when A is reached, meaning A is *never attempted at all* in that scenario, confirming Codex's claim: A would never be counted succeeded today, but is in the new design. This is a materially different and broader case than the same-pass fold-in already documented, since it involves folios being attempted at all in an order/timing today's design would never permit, not merely a difference in how many attempts a single already-in-flight folio gets. **Resolved**: AC-7's exception paragraph, the "bit-identical...except" carve-out, and AC-7.1(g) were all broadened to explicitly cover both the same-pass and cross-pass cases (see above).
- **"System-wide memory exhaustion" overstated the trigger condition for `-ENOMEM`.** Codex correctly noted `-ENOMEM` here is typically a destination-folio allocation failure from `get_new_folio()` under node/zone/policy/cgroup/GFP-mask constraints, not necessarily a whole-system OOM condition. **Resolved**: reworded to "destination-allocation-failure/error-path outcome" throughout the exception text and the Round-4 rationale above.
- **Other plan text still made unqualified "no functional change" claims that now contradict the accepted exception.** Flagged three sites: the Goal Description's "pure structural/readability refactor with no functional change intended", AC-6's negative test rejecting final-count differences "for any given input", and Milestone 4's "Verify no functional change". Verified all three existed as flagged by grepping the plan directly. **Resolved**: Goal Description now states the refactor is "overwhelmingly" a no-functional-change refactor with one named, cross-referenced exception; AC-6's negative test is scoped to inputs that don't trigger the AC-7 exception (and clarifies AC-6's own merge-arithmetic correctness is unaffected either way, since the divergence originates in AC-7's fallback-loop *ordering*, not in `gather_migrate_pages_stats()`'s field arithmetic); Milestone 4 now reads "Verify no functional change (outside the single accepted exception in AC-7)".
- Also confirmed correct, no fix needed: the same-pass example's accuracy, the "accounting bug vs. behavior change" distinction, and that outside of any `-ENOMEM` occurring during the sync fallback, the two loop shapes still reach identical final per-folio outcomes (this was verified, not merely asserted, when broadening the exception text — no other divergence source was found).

**Not yet re-verified by a further Codex round** against this round-5-corrected, broadened version.

### Codex Round 6 on Parts 2-4 (findings and verification)

A sixth Codex round reviewed the round-5-broadened exception text (both the same-pass and cross-pass cases in AC-7 and AC-7.1(g)), the corrected "destination-allocation-failure" wording, and the three newly-qualified sections (Goal Description, AC-6, Milestone 4). It agreed the broadened exception correctly captures the two main `-ENOMEM`-triggered scenarios and that AC-7.1(g) remains a real, checkable accounting point — but found two further gaps, both confirmed:

- **The cross-pass case's wording was itself imprecise about what a later-listed folio's "final outcome" can be before the earlier folio's later `-ENOMEM`.** I had written "success or exhausted-`-EAGAIN` failure" — but "exhausted-`-EAGAIN`" accounting (`nr_failed += retry` etc. at the bottom of the fallback loop, mirroring mm/migrate.c-style pass-limit exhaustion) only fires once, globally, for every folio still marked `-EAGAIN` together, at the point the bounded `for (pass...)` loop itself terminates (either `retry == 0` or `pass == NR_MAX_MIGRATE_SYNC_RETRY`) — verified directly by re-reading the `migrate_folios()` sketch's fallback loop (Part 4 code above, `nr_failed += retry; ...; return nr_failed;` after the `for` loop exits): a specific folio cannot have already been charged as "exhausted" while the loop is still running to reach a later folio's `-ENOMEM`, since reaching that `-ENOMEM` requires the loop to still be executing, which is only possible if the loop has not yet exhausted. Before a later `-ENOMEM`, a folio can only have succeeded or hit a genuinely final (non-retry, non-`-ENOMEM`) permanent-failure path — a precheck-handled outcome, or `migrate_folio_sync_one()`'s own `default:`/post-move permanent-failure branch. **Resolved**: reworded both the AC-7 cross-pass description and AC-7.1(g) to say "success, or a permanent failure via a precheck-handled path or `migrate_folio_sync_one()`'s own non-`-EAGAIN`/non-`-ENOMEM` failure branch" instead of "exhausted-`-EAGAIN` failure," with an explicit note on why exhausted-retry accounting cannot fire mid-loop for a single folio (see above).
- **A further, non-`-ENOMEM` divergence: returned-folio list ordering.** Codex noted that even without any `-ENOMEM` occurring, the physical order in which failed/not-migrated folios end up in `ret_folios` (and, transitively, in the caller's `from` list after `migrate_pages()`'s final `list_splice(&ret_folios, from)`, mm/migrate.c:2172) can differ between the round-robin and sequential-to-completion loop shapes, purely because folios finalize in a different relative order under the two designs. Verified: counts and each folio's own outcome are unaffected — this is a pure list-ordering effect, not an accounting or outcome divergence — and confirmed nothing downstream depends on or documents an ordering guarantee for that returned list (`putback_movable_pages()` and other consumers are order-independent). **Resolved**: added as a third, explicitly cosmetic bullet in AC-7's exception list, noted as having no behavioral consequence, to keep the "bit-identical...except" claim honest at the strictest possible reading rather than silently leaving it uncovered.
- Confirmed correct, no further fix needed: the broadened same-pass/cross-pass exception's core scope, the "destination-allocation-failure/error-path outcome" rewording, and the internal consistency of Goal Description/AC-6/Milestone 4 with the named AC-7 exception.

**Not yet re-verified by a further Codex round** against this round-6-corrected version.

### Codex Round 7 on Parts 2-4 (findings and verification)

A seventh Codex round did a skeptical final pass over the full round-6-broadened exception text, explicitly asked to check for any remaining scenario (three-plus-folio interactions, split-path interactions, downstream ordering dependencies) not yet covered. It confirmed the same-pass/cross-pass exception text, the exhausted-`-EAGAIN` correction, and the Goal Description/AC-6/Milestone 4 qualifications were all now accurate — but surfaced one genuinely new, more substantive finding, independently verified before being accepted (not taken on Codex's word, matching the rigor applied throughout):

- **`migrate_folio_sync_one()`'s move-step `-EAGAIN` handling is not just reordered relative to today's code — it does strictly more allocation/unmap work per retry, and carries additional `-ENOMEM` exposure that has nothing to do with multi-folio ordering.** Verified directly by reading `migrate_folios_move()` and its caller loop in full (mm/migrate.c:1727-1777, 1994-2012): today's `migrate_pages_batch()` runs a *dedicated* move-phase `for (pass...)` loop that retries a `migrate_folio_move()` `-EAGAIN` **in place against the same already-allocated `dst`**, with no re-unmap and no new `get_new_folio()` call — and this applies even to a singleton list, since `migrate_pages_sync()`'s fallback invokes the full, unmodified `migrate_pages_batch()` per folio. Also verified `migrate_folio_undo_dst()` (mm/migrate.c:1188-1197): it calls `put_new_folio()`/`folio_put(dst)`, genuinely releasing the destination folio, confirming that `migrate_folio_sync_one()`'s unwind-on-`-EAGAIN` does not preserve `dst` — the next outer-pass retry allocates a fresh one via a fresh `migrate_folio_unmap()` call. Net effect: a single folio's own move-retry, under the new design, can hit `-ENOMEM` on a subsequent allocation attempt in a scenario where today's code — reusing its one already-successful allocation — would have kept retrying the move and eventually succeeded. This is distinct in kind from rounds 4-6's multi-folio ordering findings: it affects even a `from` list of exactly one folio, with no other folio involved at all.
  - Also flagged (smaller, folded into the same fix): the cross-pass wording's "success or permanent failure" list for a later-listed folio's possible outcome should also explicitly include the split-success path (`nr_split`/`nr_thp_split`, moved to `split_folios`), which is a third distinct final-outcome category alongside success and permanent failure.
- **Presented to the repository owner as a decision point** (not unilaterally resolved), since — unlike rounds 5-6's pure wording precision — this is a substantive behavioral tradeoff with a real design alternative: accept the extra allocation churn/`-ENOMEM` exposure as a further documented exception (keeping `migrate_folio_sync_one()` a clean, fully self-contained, single-attempt primitive — the same property that lets it mirror `unmap_and_move_hugetlb_folio()`'s shape), or redesign the primitive/fallback loop to preserve `dst` across move-step retries (reintroducing cross-call state threading that the primitive was specifically designed to avoid). **Resolved by the repository owner**: accept as a documented exception, consistent with the earlier, more general decision to restore pre-batching `__unmap_and_move()` semantics (which also never preserved in-place retry state) for this path rather than reproduce every batching-era optimization. AC-7's exception paragraph gained a fourth bullet ("Move-step `-EAGAIN` retry economy"), AC-7.1 gained a new point (h) alongside the retry-economy disclaimer, and the "bit-identical...except" carve-out and Positive/Negative test counts were updated from seven to eight points accordingly.

**Not yet re-verified by a further Codex round** against this round-7-corrected version.

### Codex Round 8 on Parts 2-4 (findings and verification)

An eighth Codex round reviewed the round-7 retry-economy exception and its resolution as an accepted, documented change. It confirmed the core technical claims independently — the in-place same-`dst` retry mechanism in `migrate_folios_move()`, `migrate_folio_undo_dst()`'s genuine release of `dst`, AC-7.1(h)'s meaningfulness and consistency with (d), and no gap at the async-pre-pass boundary or in the `-ENOMEM`-then-split/`MR_LONGTERM_PIN` paths (no destination folio exists yet at those points, so no analogous same-`dst`-preservation question arises there) — and explicitly stated **no code-mechanics issue remains and no redesign of `migrate_folio_sync_one()` is needed**; the round-7 decision itself was not reopened. It did find the round-7 documentation had scoped the retry-economy divergence too narrowly, all confirmed:

- **The retry-economy divergence isn't only about `-ENOMEM` exposure.** The round-7 wording focused on "the fresh `get_new_folio()` call could itself return `NULL`," but re-allocating and re-unmapping on every move-step `-EAGAIN` retry also repeats source-locking and destination-locking work that today's in-place retry (reusing its already-locked `dst`) never repeats — so the new design can also hit a fresh `-EAGAIN` from `migrate_folio_unmap()` itself (e.g., lock contention on the newly-allocated `dst`) in a scenario where today's retry, holding its one `dst` locked throughout, would not. **Resolved**: the retry-economy bullet in AC-7, AC-7.1(h), and every "exception triggered by `-ENOMEM`" summary elsewhere in the plan were broadened to say the retry-economy divergence is triggered by *any* move-step `-EAGAIN` — regardless of whether the subsequent retry succeeds, fails via fresh `-ENOMEM`, or fails via a fresh `-EAGAIN` — not narrowly by `-ENOMEM` alone.
- **Several sections that had been correctly qualified for the ordering exception (rounds 5-6) went stale again once the retry-economy exception was added in round 7**, since they still said "when a `-ENOMEM` occurs" without also covering "when a move-step `-EAGAIN` occurs." Flagged: the Goal Description's exception summary, AC-7's post-bullet summary paragraph, the "bit-identical...except" carve-out, and AC-6's negative-test parenthetical. **Resolved**: all four reworded to name both triggers (a `-ENOMEM` anywhere in the sync fallback, or a move-step `-EAGAIN` on any folio) rather than only `-ENOMEM`.
- Confirmed correct, no further fix needed: the async pre-pass is unaffected (it still uses the unmodified `migrate_folios_batch()`, preserving today's in-place move-retry there), and there is no additional cross-boundary `dst`-state-preservation gap between the async pre-pass and the sync fallback (today's code already fully releases any pre-pass `dst` state before the fallback begins, so the new design isn't losing something today's code would have carried across that specific boundary).

**Not yet re-verified by a further Codex round** against this round-8-corrected version.

### Codex Round 9 on Parts 2-4 (findings and verification)

A ninth Codex round did a full-plan skeptical pass, not just AC-7/AC-7.1. It explicitly confirmed the design itself is technically converged — no code-mechanics issue, no redesign needed, the core claims underlying rounds 4-8 all independently re-verified as sound — and found only remaining documentation-scope issues, all confirmed:

- **The post-bullet summary in AC-7 mis-scoped the cosmetic returned-list-ordering effect as confined to `-ENOMEM`, contradicting its own bullet.** The returned-list-ordering bullet correctly says it can arise from any relative finalization-order difference; a plain retryable `-EAGAIN` from the unmap phase or `migrate_folio_precheck()` (not only a move-step `-EAGAIN`, and with no `-ENOMEM` involved at all) is enough to reorder which folio finalizes first, hence enough to reorder the returned-folio list — but the summary paragraph had implicitly folded it under the `-ENOMEM`-triggered bucket. **Resolved**: reworded the summary to name three distinct triggers — the `-ENOMEM`-triggered substantive outcome/count divergences, the move-step-`-EAGAIN`-triggered retry-economy divergence, and the broader, any-`-EAGAIN`-triggered cosmetic list-ordering effect (never a count/outcome divergence on its own) — and clarified that "outcomes and counts" (not "behavior" unqualified) are what's identical outside the first two triggers.
- **`task17` in Task Breakdown still described the pre-round-4 accounting-only review scope**, not mentioning AC-7.1(g)/(h) or the accepted exceptions at all. **Resolved**: reworded to explicitly reference AC-7.1(a)-(h), including the same-pass/cross-pass fold-in accounting (g) and the accepted retry-economy divergence (h).
- **The retry-economy discussion hadn't mentioned the source-side remap/unlock cycle.** Codex noted that the full unwind between move-step retries doesn't only release `dst` — it also restores migration PTEs and unlocks `src` via `migrate_folio_undo_src()`, so a retried folio's source-side mapped/locked state cycles unmapped→remapped→unmapped again across retries, unlike today's design where `src` stays unmapped and `dst` stays locked continuously. Also asked whether this could affect committed success-path accounting (memcg charging, NUMA balancing counters). Verified: `migrate_folio_done()`'s success-path counters (`count_vm_numa_events()`, `count_memcg_events()`, etc., mm/migrate.c:~2771-2774) only fire once, on the attempt that actually succeeds — identical in both designs; the extra remap/unmap cycling only touches *failed* intermediate attempts, which were never counted as success in either design, so no accounting divergence follows from this. **Resolved**: added a paragraph documenting the remap/unlock cycle and this success-path-accounting confirmation directly under AC-7's exception text.
- Also confirmed correct, no further fix needed: Path Boundaries, Dependencies and Sequence (other than task17), DEC-1, and Implementation Notes — none of them assume strict full equivalence unaware of the accepted exceptions; they either don't touch this topic or already defer to AC-7/AC-7.1.

**Not yet re-verified by a further Codex round** against this round-9-corrected version. Given round 9 explicitly stated the design itself is converged and the last three rounds (7, 8, 9) have progressively narrowed to documentation-scope-only findings (each smaller in surface than the last), this is judged to be at or very near genuine convergence — see Convergence Status for the recommendation on whether a further round is warranted.

### Part 3 design refinement: per-engine local, merge at the engine's own return (direct discussion, post-round-9)

After round 9, the repository owner asked whether `gather_migrate_pages_stats()` could be folded into every `migrate_*()` function — i.e., have each engine update stats internally rather than the caller doing the merge. Claude's first-pass answer (verified by grepping every `stats->` write in the sketch, confirming all are purely additive with no read-back) was that the per-engine local could be eliminated *entirely*, passing `&stats` (the grand total) straight into every engine with no local or merge step anywhere. The repository owner refined this: **keep each engine's own local stats struct, but move the merge call to the engine's own return point(s)**, rather than either (a) the caller managing a per-engine local + merge (the prior AC-6 design) or (b) eliminating the local altogether.

This is adopted as a genuine design improvement, not just a stylistic preference: it gives every engine an identical, symmetric external contract (`migrate_X(..., &stats, ...)`, no caller-side bookkeeping) while preserving per-engine accounting isolation internally (useful if a future contributor wants to inspect one engine's contribution before it's merged). A relevant finding surfaced along the way: today's *actual*, unmodified `migrate_hugetlbs()` (mm/migrate.c:2116-2117) already takes `&stats` (the single master struct declared in `migrate_pages()`) directly, with no local and no merge at all — meaning the *previous* AC-6 draft's `hugetlb_stats`-local-at-the-call-site pattern didn't even match today's real precedent for hugetlb; it was invented for symmetry with the new `movable_stats`. The newly-adopted pattern requires a small, mechanical, log-only change to `migrate_hugetlbs()` (redirect its existing writes to a new internal local, add a merge call at each of its 2 return points) that wasn't previously in this plan's scope — its retry/accounting *logic* is untouched.

**Applied to the plan**: AC-6 rewritten to describe the new contract (each engine owns its local, merges at its own return points; `migrate_pages()` declares only the one `stats` and never merges directly). Part 2's `migrate_movable_ops_pages()` sketch updated to add `local_stats` + two merge calls (one per return point). Part 3 given a new paragraph documenting the pattern and `migrate_hugetlbs()`'s small addition. Part 4's `migrate_folios()` sketch updated to add `folio_local_stats` (distinct from the existing, still-necessary field-selective `pass_stats` scratch) + three merge calls (one per return point: the async/`-ENOMEM` early return, the fallback loop's `-ENOMEM` fatal return, and the final normal return). The `migrate_pages()` top-level sketch simplified to remove all three per-engine locals and four explicit `gather_migrate_pages_stats()` calls, threading `&stats` directly into every engine call and into the one-shot `split_folios` follow-up call. Milestone 3 and tasks 3, 6, 10, 11 updated to reflect where each piece of this now lives.

### Codex Round 10 on Parts 2-4 (Part 3 design refinement, findings and verification)

A tenth Codex round specifically reviewed the Part 3 refinement above (callee-owned local stats, merge at each engine's own return point) — a real code-shape change, not documentation-scoping. It confirmed every element: all three engines' return paths each have exactly one merge call (2 for `migrate_hugetlbs()`, 2 for `migrate_movable_ops_pages()`, 3 for `migrate_folios()`), `migrate_hugetlbs()`'s change is purely a mechanical accounting-target redirect with no retry/list/control-flow change, `migrate_folios()`'s `pass_stats` is still correctly field-selectively merged into `folio_local_stats` (not the caller's grand total), the `migrate_folio_sync_one()` call sites correctly target `&folio_local_stats`, the `split_folios` follow-up's direct write to `&stats` is safe (runs strictly after `migrate_folios()` has already merged its own contribution), the reorganization is purely a "where does the merge call live" change with no arithmetic difference from the pre-round-10 caller-local design, and it does not interact badly with the already-accepted AC-7 exceptions (the `-ENOMEM` abort merge happens at the same logical boundary — after the engine finalizes its local accounting, before the caller observes the return — regardless of which side owns the merge call). **REQUIRED_CHANGES: none. CONVERGENCE_VERDICT: converged.** One optional wording tightening was applied (AC-6's "no other merge logic" clarified to "no other whole-struct additive merge logic," explicitly carving out the still-separate field-selective `pass_stats` merge).

### Codex Round 11: full-plan holistic review (findings and verification)

An eleventh Codex round did a fresh, broad pass over the *entire* document — not limited to the areas rounds 1-10 already scrutinized (which concentrated on Part 4's retry semantics and, in round 10, Part 3's stats-ownership redesign). It re-verified the plan's structural claims directly against the current `mm/migrate.c` and checked cross-document consistency (Goal Description, AC-1 through AC-7.1, Path Boundaries, Feasibility Hints sketches, Dependencies and Sequence, Task Breakdown). It confirmed no stale `gather_migrate_pages_stats` naming gaps, correct `try_to_unmap_flush()` conditionality (AC-2), a coherent round-10 stats design, an accurate AC-7 exception summary, and a correct `migrate_folio_sync_one()` `-EAGAIN`-unwind sketch — all re-confirmations of prior rounds' work, not new. It found three genuinely new issues, all independently verified before being accepted:

- **Part 2's `migrate_one_movable_ops_page()` sketch omits `migrate_folio_unmap()`'s `folio_test_writeback(src)` wait/`-EBUSY` gate** (mm/migrate.c:1266-1281), which today runs on every folio — including `movable_ops` ones — before the `page_has_movable_ops()` branch (mm/migrate.c:1312), and which AC-5.1 didn't previously mention as either preserved or deliberately dropped. **Independently verified, not taken on Codex's word**: read `page_has_movable_ops()`'s exact definition (`PageMovableOps() && (PageOffline() || PageZsmalloc())`, include/linux/page-flags.h:1122-1126), then grepped both providers (`mm/balloon.c`'s `balloon_mops`, `mm/zsmalloc.c`'s `zsmalloc_mops`) and every other `__SetPageOffline()` caller in the tree (virtio_mem, vmw_balloon, hv_balloon, xen balloon, x86 `coco/sev`, powerpc `memtrace`, `mm_init.c`'s reserved-page marking) for any writeback-machinery usage — none exists; these pages are never page-cache- or swap-cache-backed, so `PG_writeback` can structurally never be set on one. **Resolved**: confirmed this is unreachable dead code for this exact subset of folios, not a behavior change — documented explicitly as an invariant in both the Part 2 sketch (inline comment) and AC-5.1 (prose), rather than either reproducing genuinely-inapplicable logic or leaving the omission unexplained.
- **DEC-1's status was stated inconsistently**: Convergence Status said "remains pending," while Pending User Decisions' own DEC-1 entry said "Decision Status: Deferred — repository owner confirmed." **Resolved**: reworded Convergence Status to state DEC-1 is resolved-as-deferred and non-blocking, matching its own entry — no actual disagreement existed, only inconsistent wording between the two references to it.
- **`task2` (commit messages for each part, AC-4) depended only on `task1` (Part 1)**, even though AC-4 requires one commit message per part and Part 2/3/4's code (and thus their rationale) doesn't exist until later tasks land. **Resolved**: `task2`'s dependencies extended to `task1, task5, task11` (each part's final implementation task), matching the dependency shape already implied by `task12` (checkpatch), which correctly depends on the same three.

**REQUIRED_CHANGES from this round: all three applied above. CONVERGENCE_VERDICT: partially_converged at the time of the review, now resolved by the fixes above** — no code-mechanics issue was found in Parts 1, 3, or 4; the one substantive finding (writeback) was in Part 2, an area that had received comparatively less dedicated scrutiny than Part 4's retry logic in prior rounds.

### Convergence Status

- **Part 1** (AC-1–AC-4, the `migrate_folios_unmap()` extraction): `converged` — 3 Codex rounds, empty final REQUIRED_CHANGES, no high-impact DISAGREE. (Note: Milestone 3/Phase C makes a small subsequent internal change to this same function, extracting `migrate_folio_precheck()` — this does not reopen AC-1/AC-2's external-behavior guarantees, which concern the unmap-vs-move boundary, not the pre-unmap checks' internal organization.)
- **Parts 2-4** (AC-5–AC-7.1): eleven Codex rounds completed. Rounds 1-9 (see below) converged the folio sync fallback's retry-loop design and its accepted exceptions in AC-7/AC-7.1, ending with round 9 confirming the design itself technically converged. Round 10 reviewed a subsequent, real (non-documentation) Part 3/AC-6 design refinement — moving stats-merge ownership from the caller (`migrate_pages()`) into each engine (callee-owned local, merged at the engine's own return points), prompted by a repository-owner design question — and returned an explicit `converged` verdict with empty `REQUIRED_CHANGES` and only one optional wording nit (applied). Round 11 was a fresh, full-plan holistic pass (not limited to Part 4's or Part 3's previously-scrutinized areas) and found one genuinely new, independently-verified issue in Part 2 (`migrate_one_movable_ops_page()` omitting a writeback gate that is provably unreachable dead code for `movable_ops` pages — resolved by documenting the invariant explicitly rather than reproducing dead code) plus two document-consistency nits (a DEC-1 wording contradiction, a stale `task2` dependency) — all three fixed, with **REQUIRED_CHANGES from round 11 now fully applied**. **Parts 2-4 are now judged converged**, matching Part 1's status, pending only the repository owner's own final read-through (and, given round 11 found a real issue in an area prior rounds hadn't focused on, a further round remains a reasonable option if the repository owner wants additional confidence before implementation). Round-by-round history: not a rubber-stamp process at any point — Round 1: the fundamentally broken naive `migrate_folio_sync_one()` composition, the wrong `nr_pass` constant, the wholesale stats merge, the `migrate_folio_done()` guard, the movable success-path cleanup, movable lock-acquisition parity. Round 2: the missing `list_del`, the missing pre-unmap/`-ENOMEM` logic (resolved via the `migrate_folio_precheck()` extraction), the movable_ops mode-sequencing question (resolved by repository-owner decision, not a code fix). Round 3: `-ENOMEM` not being fatal, the same-pass `-EAGAIN`-then-`-ENOMEM` fold-in, the missing `nosplit` guard, the `cond_resched` variant, a compile-level type bug. Round 4: surfaced the shared-pass round-robin retry ordering as a genuine divergence from today's per-folio-to-exhaustion retry opportunity under `-ENOMEM` — **resolved by repository-owner decision** to accept as an intentional, documented behavior change. Round 5: broadened the same-pass-only documentation to also cover a cross-pass ordering case, corrected an overstated "system-wide memory exhaustion" characterization, and qualified three other plan sections (Goal Description, AC-6, Milestone 4). Round 6: corrected an imprecise "exhausted-`-EAGAIN`" phrase in the cross-pass wording and documented a further, purely cosmetic divergence (returned-folio list ordering). Round 7: surfaced a genuinely new, substantive finding — `migrate_folio_sync_one()`'s move-step `-EAGAIN` unwind releases `dst` and re-allocates fresh on retry, where today's code (even for singleton sync-fallback calls) retries in place against the same already-allocated `dst` — **presented to the repository owner as a decision point (accept as exception vs. redesign to preserve `dst`) and resolved by accepting it as a further documented exception**, consistent with the earlier decision to restore pre-batching retry semantics for this path; AC-7 gained a fourth exception bullet and AC-7.1 gained point (h). Round 8: confirmed round 7's core finding and decision needed no reopening, but found the retry-economy exception's documentation was itself scoped too narrowly (limited to `-ENOMEM` exposure when it's actually triggered by any move-step `-EAGAIN`) and four sections had gone stale again — resolved by broadening them. Round 9: a full-plan pass, explicitly confirming the design itself is technically converged (no code-mechanics issue, no redesign needed) — remaining findings were purely documentation-scope: the AC-7 summary had mis-scoped the cosmetic returned-list-ordering effect as `-ENOMEM`-only (it isn't — any `-EAGAIN` interleaving triggers it), `task17` still described the pre-round-4 review scope, and the retry-economy discussion hadn't mentioned the source-side remap/unlock cycle between move-step retries (verified to have no effect on success-path accounting, since success counters only fire once, on the attempt that actually succeeds, in both designs). **Resolved**: reworded the AC-7 summary to name three distinct triggers, updated `task17` to reference AC-7.1(a)-(h), and added a paragraph on the remap/unlock cycle plus the success-accounting confirmation. No outstanding `REQUIRED_CHANGES` remain from round 9. Round 10: reviewed the callee-owned-local-stats Part 3 refinement (see the dedicated Deliberation subsection above) and returned `converged` with empty `REQUIRED_CHANGES`. Round 11: a full-plan holistic pass (see the dedicated Deliberation subsection above) found and resolved a real Part 2 writeback-gate documentation gap plus two consistency nits (DEC-1 wording, `task2` dependencies).
- DEC-1 is resolved (deferred) and non-blocking — see Pending User Decisions below; its `Decision Status: Deferred` reflects the repository owner's confirmed choice, not an open question.

## Pending User Decisions

- DEC-1: Should the `struct migrate_control` parameter-consolidation refactor (bundling `get_new_folio`/`put_new_folio`/`private`/`mode`/`reason` across the various internal functions) be pursued now as an immediate follow-up, or deferred indefinitely as a separately-scoped future plan?
  - Claude Position: Defer — it's a separate, larger structural change on a different axis (parameter bundling) from this plan's stats-splitting (AC-6), which deliberately goes the *other* direction (more separate structs). Combining them would be confusing to review together.
  - Codex Position: (from Part 1's round-1 review) Defer; recommended not doing it now given added review-risk surface, explicitly flagged as the repository owner's call.
  - Tradeoff Summary: Unaffected by this redesign either way — this decision was already made once (deferred) and nothing in Parts 2-4 changes that reasoning.
  - Decision Status: Deferred — repository owner confirmed pursuing `struct migrate_control` (if at all) as a separate future plan.

## Implementation Notes

### Code Style Requirements

- Implementation code and comments must NOT contain plan-specific terminology such as "AC-", "Milestone", "Step", "Phase", "Part 1/2/3/4", or similar workflow markers. These terms are for plan documentation only. Use descriptive, domain-appropriate naming in code instead.
- Code comments near `migrate_folios_unmap()`, `migrate_movable_ops_pages()`, `migrate_folios()`, etc. must describe current behavior only — do not reference external RFC threads, DMA engines, offload mechanisms, or this plan's "Part N" numbering in code comments (that context belongs in commit messages, not the code).
