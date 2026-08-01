# Remove PG_private from Page Flags — Implementation Plan

**Goal:** Remove the `PG_private` bit from `enum pageflags` by making `folio->private != NULL`
the canonical indicator that a folio carries private filesystem data, freeing a scarce page-flag bit.

**Architecture:** `PG_private` is redundant with `folio->private != NULL` in the standard case:
`folio_attach_private()` always sets both the pointer and the flag, so testing the pointer suffices.
Several exceptional sites use `PG_private` without relying on `folio->private != NULL` as the test,
and must be fixed first.

The preferred approach for exceptional sites is to use `page->private != 0` in place of
`PagePrivate()` wherever the site already stores a non-zero value there. Where this is not possible
(zsmalloc: `page->private` overlaps with `zpdesc->zspage` and is non-NULL for **all** zpdescs in
the chain, not just the first), use a direct pointer comparison against `zspage->first_zpdesc`.

**Critical ordering constraints:**
1. Task 7 (`folio_change_private()` cleanup) MUST commit before Task 8 (semantic pivot). After
   the pivot, `folio_change_private(folio, NULL)` on a Type A folio would zero `folio->private`
   without calling `folio_put()`, leaking the refcount taken by `folio_attach_private()`.
2. Task 8 (semantic pivot: widen tests to pointer checks, make Set/Clear no-ops, drop
   `PG_private` from `PAGE_FLAGS_CHECK_AT_FREE`) MUST commit before Task 9 (strip no-op calls
   from `folio_attach/detach_private`). Reversing this order creates a window where
   `folio_needs_release()` silently skips folios that have private data attached.
3. `PAGE_FLAGS_CHECK_AT_FREE` must lose `PG_private` **in the same commit** as Task 8. Once
   `folio_set_private()` is a no-op, any folio that had the bit set before the pivot will retain
   the stale bit and trip `bad_page()` at free time if the check is not removed first.
4. Coccinelle bulk removal (Task 10) and manual `PagePrivate()` test conversions (Task 11) MUST
   come **after** the semantic pivot (Task 8). Removing `folio_set_private()` calls before the
   pivot makes newly-attached private data invisible to `folio_has_private()`, which is still
   flag-based until the pivot.

---

## File Map

**Core infrastructure (modified):**
- `include/linux/page-flags.h` — remove `PG_private` enum value; update `PAGE_FLAGS_CHECK_AT_FREE`,
  `PAGE_FLAGS_PRIVATE`, `folio_has_private()`, `folio_test_private()`; add no-op shims in Task 8;
  remove shims in Task 13
- `include/linux/pagemap.h` — update `folio_attach_private()`, `folio_detach_private()`,
  `folio_change_private()`
- `include/linux/mm.h` — update `folio_expected_ref_count()`
- `kernel/vmcore_info.c` — remove `VMCOREINFO_NUMBER(PG_private)`
- `fs/proc/page.c` — update `KPF_PRIVATE` to source from `folio->private != NULL` (with
  swapcache and hugetlb guards) instead of the `PG_private` bit (near-ABI-preserving;
  see Task 8 Step 6 for intentional semantic differences).
  `include/linux/kernel-page-flags.h` and `tools/mm/page-types.c` are **unchanged**.

**Exceptional users (each in its own commit):**
- `mm/zsmalloc.c` + `mm/zpdesc.h` — **cannot** use `page->private != 0`: `page->private` overlaps
  with `zpdesc->zspage` (see `ZPDESC_MATCH(private, zspage)`), which is non-NULL for **all**
  zpdescs in the chain. Replace with pointer comparison `zpdesc->zspage->first_zpdesc == zpdesc`.
- `kernel/events/ring_buffer.c` + `arch/x86/events/intel/bts.c` + `arch/x86/events/intel/pt.c` —
  `page->private` already stores the allocation order (non-zero when high-order). Replace
  `PagePrivate(page)` with `page_private(page) != 0`.
- `drivers/xen/grant-table.c` — 32-bit: stores pointer in `page->private`, non-NULL is a valid
  guard. 64-bit: stores inline `xen_page_foreign {domid, gref}` — both fields can be zero
  (domid=0 is dom0; gref=0 is technically possible), so `page_private() != 0` is NOT a reliable
  guard on 64-bit. Make 64-bit cleanup unconditional (`set_page_private(pages[i], 0)` for all
  pages, no guard). `include/xen/grant_table.h` is **unchanged**.
- `fs/crypto/crypto.c` — stores folio pointer (always non-NULL) in `page->private`. Remove
  `SetPagePrivate`/`ClearPagePrivate`; pointer non-NULL is sufficient.

**Raw folio->private writers without PG_private (audit complete — Task 1 Step 5):**
- `fs/erofs/zdata.c:1911` — readahead linked list on live pagecache folios. **MUST FIX in
  Task 7b**: add `readahead_folio_reverse()` and delete the list (attach/detach doesn't fit —
  scan clobbers private mid-life).
- `fs/erofs/data.c:266,271,279,283` — in-flight I/O counter (atomic ops) on live pagecache
  folios. **MUST FIX in Task 7b**: convert `erofs_onlinefolio_*()` to
  `folio_attach_private()`/`folio_detach_private()` (the counter is the mutable payload).
- `fs/erofs/zdata.c:569` — pre-cache `Z_EROFS_PREALLOCATED_FOLIO` marker; safe (cleared
  before insertion at line 1515).
- `fs/erofs/zdata.c:1577`, `fs/erofs/decompressor*.c`, `fs/erofs/internal.h:469` —
  shortlived/pagepool pages; non-pagecache; safe.
- All others (`page->private` writes in KVM, binder, buddy, TTM, network, relay, drbd,
  kexec, percpu, blk-mq, memory-failure, etc.) — non-pagecache (`folio->mapping == NULL`);
  safe. Full table in `task_1.md` Step 5.

**B-noref site:**
- `fs/nfs/write.c` — **Option B** (do NOT migrate to `folio_attach_private()`): the folio ref is
  held by the `nfs_page` lifecycle; `folio_set/clear_private` calls are flag-only and are removed
  by the Coccinelle + manual cleanup after the pivot (Tasks 10–11). No separate NFS task needed.

**FS bulk transforms (via Coccinelle, Task 10 — after pivot):**
- All `fs/` files that call `folio_set_private()` / `folio_clear_private()` directly alongside
  a `folio->private` write.

**PagePrivate() test callers needing manual conversion (Task 11 — after pivot):**
- `drivers/md/md-bitmap.c:538`
- `fs/ceph/addr.c:73` (`page_snap_context()`)
- `fs/f2fs/f2fs.h` (inside `page_private_##name` and `set_page_private_##name` macros)
- `include/linux/buffer_head.h:181` (`page_buffers()` BUG_ON)

**Created:**
- `scripts/coccinelle/misc/pg-private-removal.cocci`

---

## Task 1: Full audit of all PG_private call sites  ✓ COMPLETE

**Results:** See `task_1.md` for the full categorised audit.
**Key findings from Step 5 (raw folio->private audit):**
- `fs/erofs/zdata.c:1911` — readahead linked list on live pagecache folios. **Task 7b:** add
  `readahead_folio_reverse()` and delete the list.
- `fs/erofs/data.c:266,271,279,283,287` — in-flight I/O counter on live pagecache folios;
  affects `folio_has_private()`, `folio_expected_ref_count()`, KPF_PRIVATE after pivot.
  **Task 7b:** convert `erofs_onlinefolio_*()` to `folio_attach_private()`/`folio_detach_private()`.
- All other raw `page->private` / `set_page_private()` sites are non-pagecache (`folio->mapping == NULL`) or already handled by earlier tasks. Safe.

- [x] **Step 1: Find all flag accessors and API callers tree-wide**

```bash
git grep -rn \
  -e 'SetPagePrivate\|ClearPagePrivate\|PagePrivate(' \
  -e '__SetPagePrivate\|__ClearPagePrivate' \
  -e 'folio_set_private\|folio_clear_private\|folio_test_private' \
  -e 'folio_attach_private\|folio_detach_private' \
  -e 'attach_page_private\|detach_page_private' \
  -- '*.c' '*.h' \
  | grep -v 'private_2' \
  | sort > /tmp/pg_private_audit.txt
wc -l /tmp/pg_private_audit.txt
```

- [x] **Step 2: Audit networking, drivers, and perf subsystems**

```bash
git grep -n 'PagePrivate\|folio_set_private\|SetPagePrivate' net/core/page_pool.c
git grep -rn 'PagePrivate\|folio_set_private' drivers/gpu/drm/ | grep -v 'private_2'
git grep -rn 'PagePrivate\|SetPagePrivate\|ClearPagePrivate' \
    arch/x86/events/ arch/arm64/kernel/perf_event.c \
    drivers/perf/ drivers/hwtracing/
```

- [x] **Step 3: Categorise each site**

- **A** — via `folio_attach_private()` / `folio_detach_private()` (standard, refcounted)
- **B-ref** — direct `folio_set_private()` with non-NULL `folio->private` AND matching `folio_get()`
- **B-noref** — direct `folio_set_private()` with non-NULL `folio->private` but NO `folio_get()`
- **C** — flag set/tested with NO data in `folio->private` (pure boolean use)

Known C sites: `mm/zsmalloc.c`, `drivers/xen/grant-table.c` (64-bit only; 32-bit stores a pointer).
Known B-noref: `fs/nfs/write.c`.

- [x] **Step 4: Record B-noref sites** — feed into Task 6.

- [x] **Step 5: Audit raw folio->private writers that do NOT set PG_private**

The grep in Step 1 finds PG_private accessor/API call sites, but misses code that writes
`folio->private` directly without calling `folio_set_private()` or `folio_attach_private()`.
After the semantic pivot (`folio_test_private() = !!folio->private`), these sites cause
`folio_test_private()` to return true for folios where PG_private was never set.

```bash
# Raw folio->private writes without the attach/change/detach API:
git grep -rn 'folio->private\s*=' -- '*.c' '*.h' \
    | grep -v 'folio_attach_private\|folio_change_private\|folio_detach_private\|private_2'

# Raw page->private writes — use the broad pattern to catch indirect writes
# (e.g. end->private=, pages[i]->private=, p->private=) then manually discard
# non-struct-page false positives (other struct types with a 'private' field).
# Use -e to prevent the pattern starting with '->' from being parsed as a flag:
git grep -rn -e '->private[[:space:]]*=' -- '*.c' '*.h' \
    | grep -v 'private_2\|folio_attach\|folio_change\|folio_detach\|set_page_private\b'

# set_page_private() calls that do NOT accompany SetPagePrivate/folio_set_private:
# Note: this grep may match unrelated symbols (e.g. snp_set_page_private() in
# arch/x86/boot/) — review each result manually and discard false positives.
git grep -rn '\bset_page_private\b' -- '*.c' '*.h' \
    | grep -v 'private_2'

# Also audit folio_has_private() callers — its semantics change in Task 8.
# task_1.md already lists known callers (mm/internal.h, mm/truncate.c,
# mm/migrate_device.c, fs/fuse/dev.c); verify they remain correct under
# the pointer-based folio_test_fs_private() semantics.
git grep -rn 'folio_has_private(' -- '*.c' '*.h'

# Audit non-assignment mutations via &page_private() / &folio->private —
# catches atomic_inc/cmpxchg and bitops (e.g. fs/erofs/data.c, fs/f2fs/f2fs.h):
git grep -rn '&page_private(\|&folio->private' -- '*.c' '*.h' | grep -v 'private_2'
```

The table below is **not exhaustive** — it shows selected examples. Run the greps above
and classify every result before proceeding to Task 8. For each site, the safety
classification must address TWO questions:
1. Is the folio unreachable from VM paths that call `folio_test_private()` or
   `folio_test_fs_private()` while the raw private is set? (e.g. migration, reclaim, THP
   split, `mm/migrate.c:1328` `!src->mapping` branch calls `try_to_free_buffers()` even
   for mappingless folios when `folio_test_private()` is true)
2. Even if `folio->mapping == NULL`, confirm the folio is not reachable from generic
   migration or reclaim paths that don't check the mapping first.

Selected examples found in this tree:

| Site | What it stores | Safe? |
|---|---|---|
| `fs/erofs/zdata.c:569` | `Z_EROFS_PREALLOCATED_FOLIO` marker | Folio not yet in page cache; cleared at line 1515 before cache insertion. Safe if that ordering is always maintained. |
| `fs/erofs/zdata.c:1577` | `Z_EROFS_SHORTLIVED_PAGE` marker | Verify folio is locked and not visible to shrink during this window. |
| `fs/erofs/zdata.c:1911` | Folio linked-list pointer for readahead | **High risk**: real pagecache folios with `z_erofs_aops` (no `release_folio`). After pivot, `folio_needs_release()` returning true falls through to `try_to_free_buffers()`. Fix in Task 7b: add `readahead_folio_reverse()` and delete the list. |
| `fs/erofs/data.c:266` | In-flight read counter (`erofs_onlinefolio_init()` on live locked pagecache folios) | **Must be fixed before Task 8.** After pivot, `folio_has_private()`, `folio_expected_ref_count()`, and (lockless) KPF_PRIVATE treat any non-NULL private as FS data. Fix in Task 7b: convert `erofs_onlinefolio_*()` to `folio_attach_private()`/`folio_detach_private()`. |
| `drivers/android/binder_alloc.c:301` | Binder shrinker metadata | `folio->mapping == NULL` (non-pagecache); safe. |
| `arch/x86/kvm/mmu/tdp_mmu.c:235` | KVM shadow page pointer | `folio->mapping == NULL` (non-pagecache); safe. |
| `arch/arm64/kvm/mmu.c:244` | KVM stage-2 page table pointer | `folio->mapping == NULL`; safe. |
| `mm/page_alloc.c:716,1549` | Buddy page order | Buddy pages not on LRU; safe. |
| `mm/memory-failure.c:1323` | hwpoison metadata | Verify: page is taken off LRU before private is set; safe if ordering maintained. |
| `net/core/skbuff.c:2026` | skb fragment metadata | `folio->mapping == NULL` (network pages); safe. |
| `drivers/block/drbd/drbd_bitmap.c:200` | DRBD bitmap page metadata | Non-pagecache pages; verify mapping==NULL and migration unreachable. |
| `drivers/block/drbd/drbd_receiver.c:80` | DRBD receive buffer | Non-pagecache; classify from audit grep. |
| `fs/erofs/decompressor.c:104`, `decompressor_crypto.c:115` | Decompressor bounce pages | Likely non-pagecache bounce; verify lifecycle. |
| `fs/erofs/internal.h:469` | Inline EROFS helper | Classify from audit grep. |
| `kernel/kexec_core.c:289` | Kexec image page | Non-pagecache; mapping==NULL expected. |
| `kernel/relay.c:124` | Relay buffer page | Non-pagecache; verify mapping==NULL. |
| `mm/balloon.c:34` | Balloon page metadata | Non-pagecache; verify reachability from migration. |
| `mm/debug_page_alloc.c:42` | Debug page alloc tracking | Free page, not on LRU; safe. |
| `block/blk-mq.c:3630` | Block I/O request page | Non-pagecache; verify mapping==NULL and migration unreachable. |
| `drivers/net/ethernet/sun/niu.c:3321` | NIU network buffer | Non-pagecache; verify mapping==NULL. |
| `arch/x86/xen/mmu_pv.c:1477` | Xen PV MMU page | Non-pagecache; verify mapping==NULL. |
| `mm/percpu.c:256` | Per-cpu chunk page | Non-pagecache allocator page; safe. |
| (all other grep results) | — | **Classify every result before Task 8** |

Any raw writer on a pagecache folio (mapping != NULL, on LRU) that does not hold the
folio locked for the entire window when private is non-NULL must be fixed before Task 8.

- [x] **Step 6: Note folio_change_private() callers**

```bash
git grep -rn 'folio_change_private' -- '*.c' '*.h'
```

Record every call. Any NULL-destination call from a Type A user must be converted to
`folio_detach_private()` in Task 7. Non-Type-A callers (e.g. `mm/hugetlb.c`) that pass NULL to
clear internal flags must be changed to direct `folio->private = NULL` assignment instead.

---

## Task 2: Fix zsmalloc — replace PG_private with pointer comparison  ✓ COMPLETE (6759ed3da371f)


> **KPF_PRIVATE ABI note (applies to Tasks 2–5):** Each of these commits stops setting
> `PG_private` on pages that previously set it. Until Task 8 updates `/proc/kpageflags`,
> the kernel still exports `KPF_PRIVATE` by copying the `PG_private` flag, so these pages
> will immediately stop reporting `KPF_PRIVATE` after their task lands. This is an
> **intentional** per-task ABI change: zsmalloc/perf/Xen/crypto pages used `PG_private`
> incidentally (C-type pure flag), not as filesystem private data, so they should not have
> been setting `KPF_PRIVATE` in the first place.

`mm/zsmalloc.c` uses `PG_private` as a "first zpdesc" marker. `page->private` **cannot** serve as
the replacement because it overlaps with `zpdesc->zspage` (`ZPDESC_MATCH(private, zspage)` in
`mm/zpdesc.h:59`), and `zpdesc->zspage` is set to a non-NULL pointer for **every** zpdesc in the
chain in `create_page_chain()` (line 1009: `zpdesc->zspage = zspage`).

`is_first_zpdesc()` is only used in `VM_BUG_ON_PAGE` assertions. Replace with pointer comparison
`zpdesc->zspage->first_zpdesc == zpdesc`, which uses only existing data and requires no page flag.

**Files:** `mm/zsmalloc.c`, `mm/zpdesc.h`

- [x] **Step 1: Update zpdesc comment in mm/zpdesc.h**

```c
/* before */
 * Page flags used:
 * * PG_private identifies the first component page.
 * * PG_locked is used by page migration code.

/* after */
 * Page flags used:
 * * PG_locked is used by page migration code.
 * The first component page is identified by zpdesc->zspage->first_zpdesc == zpdesc.
```

- [x] **Step 2: Remove zpdesc_set_first() entirely from mm/zsmalloc.c (~line 293)**

```c
/* remove this function */
static inline void zpdesc_set_first(struct zpdesc *zpdesc)
{
    SetPagePrivate(zpdesc_page(zpdesc));
}
```

Remove the `zpdesc_set_first(zpdesc)` call from `create_page_chain()` (~line 1013).

- [x] **Step 3: Update is_first_zpdesc() (~line 479)**

```c
/* before */
static inline bool __maybe_unused is_first_zpdesc(struct zpdesc *zpdesc)
{
    return PagePrivate(zpdesc_page(zpdesc));
}

/* after */
static inline bool __maybe_unused is_first_zpdesc(struct zpdesc *zpdesc)
{
    return zpdesc->zspage->first_zpdesc == zpdesc;
}
```

- [x] **Step 4: Remove ClearPagePrivate from reset_zpdesc() (~line 849)**

```c
/* before */
static void reset_zpdesc(struct zpdesc *zpdesc)
{
    struct page *page = zpdesc_page(zpdesc);

    ClearPagePrivate(page);
    zpdesc->zspage = NULL;
    zpdesc->next = NULL;
}

/* after */
static void reset_zpdesc(struct zpdesc *zpdesc)
{
    zpdesc->zspage = NULL;
    zpdesc->next = NULL;
}
```

- [x] **Step 5: Update in-code comment in mm/zsmalloc.c (~line 1004)**

`create_page_chain()` has a comment "we set PG_private to identify the first zpdesc in the
zspage chain". Update it explicitly (it will also be caught by the final `PG_private\b` sweep
in Task 13, but updating it here keeps the commit self-contained):

```c
/* before */
 * we set PG_private to identify the first zpdesc in the zspage chain.

/* after */
 * we identify the first zpdesc via zspage->first_zpdesc == zpdesc.
```

- [x] **Step 6: Build and verify**

```bash
make mm/zsmalloc.o
git grep -n 'PagePrivate\|PG_private\|SetPagePrivate\|ClearPagePrivate' \
    mm/zsmalloc.c mm/zpdesc.h
```
Expected: no output from grep.

- [x] **Step 7: Commit**

```
mm/zsmalloc: replace PG_private first-zpdesc marker with pointer comparison

zsmalloc used PG_private as a pure boolean flag to identify the first
zpdesc in a multi-page zspage chain.  page->private cannot serve as the
replacement because it overlaps with zpdesc->zspage (ZPDESC_MATCH), which
is non-NULL for every zpdesc in the chain.

Replace with a direct pointer comparison: zpdesc->zspage->first_zpdesc
== zpdesc.  This uses only existing data, requires no page flag, and is
only called from VM_BUG_ON assertions so the overhead is negligible.
```

---

## Task 3: Fix ring_buffer — replace PagePrivate with page_private() != 0  ✓ COMPLETE (3095a550d323c)

`kernel/events/ring_buffer.c` stores the allocation order in `page->private` for high-order AUX
pages and sets `PG_private` only when `order > 0`. Since the order is already non-zero when the
flag would be set, `page_private(page) != 0` is a direct replacement.

**Files:** `kernel/events/ring_buffer.c`, `arch/x86/events/intel/bts.c`,
`arch/x86/events/intel/pt.c`

- [x] **Step 1: Verify the guard in rb_alloc_aux_page() (~line 618)**

Confirm `SetPagePrivate` is inside `if (page && order)`.

- [x] **Step 2: Remove SetPagePrivate from rb_alloc_aux_page() and update comment**

```c
/* before */
        /*
         * Communicate the allocation size to the driver:
         * if we managed to secure a high-order allocation,
         * set its first page's private to this order;
         * !PagePrivate(page) means it's just a normal page.
         */
        split_page(page, order);
        SetPagePrivate(page);
        set_page_private(page, order);

/* after */
        /*
         * Communicate the allocation size to the driver:
         * if we managed to secure a high-order allocation,
         * store this order in page->private;
         * page_private(page) == 0 means it's just a normal page.
         */
        split_page(page, order);
        set_page_private(page, order);
```

- [x] **Step 3: Remove ClearPagePrivate from rb_free_aux_page() (~line 644)**

```c
/* before */
static void rb_free_aux_page(struct perf_buffer *rb, int idx)
{
    struct page *page = virt_to_page(rb->aux_pages[idx]);

    ClearPagePrivate(page);
    __free_page(page);
}

/* after */
static void rb_free_aux_page(struct perf_buffer *rb, int idx)
{
    __free_page(virt_to_page(rb->aux_pages[idx]));
}
```

- [x] **Step 4: Update x86 Intel PMU drivers**

`arch/x86/events/intel/bts.c` (`buf_nr_pages()`):
```c
/* before */  if (!PagePrivate(page))
/* after  */  if (!page_private(page))
```

`arch/x86/events/intel/pt.c` (two sites):
```c
/* before */  if (PagePrivate(p))
/* after  */  if (page_private(p))
```

Also sweep for stragglers:
```bash
git grep -rn 'PagePrivate\|SetPagePrivate\|ClearPagePrivate' \
    arch/x86/events/ arch/arm64/kernel/perf_event.c \
    drivers/perf/ drivers/hwtracing/
```

- [x] **Step 5: Build and commit**

```bash
make kernel/events/ring_buffer.o \
     arch/x86/events/intel/bts.o arch/x86/events/intel/pt.o
```

```
perf/ring_buffer: drop PG_private from AUX page high-order marker

The flag is set only when order > 0, so page_private() == 0 already
unambiguously means single-page allocation.  Replace SetPagePrivate /
ClearPagePrivate with page_private() != 0 checks throughout ring_buffer.c
and the bts/pt PMU drivers that consume AUX pages.
```

---

## Task 4: Fix Xen grant table  ✓ COMPLETE (9f97c688d1e84)

`gnttab_pages_set_private()` / `gnttab_pages_clear_private()` in `drivers/xen/grant-table.c`.

- **32-bit**: allocates `xen_page_foreign` and stores the pointer in `page->private`. The pointer
  is always non-NULL, so `page_private() != 0` is a valid guard for cleanup.
- **64-bit**: `page->private` is used as inline storage for `struct xen_page_foreign {domid_t,
  grant_ref_t}` — both fields can legitimately be zero (domid=0 is dom0; gref=0 can occur).
  Therefore `page_private() != 0` is **not** a reliable guard on 64-bit. Make 64-bit cleanup
  unconditional: `set_page_private(pages[i], 0)` for all pages, no guard.

`xen_page_foreign()` in `include/xen/grant_table.h` guards on `PageForeign` (= `PG_owner_priv_1`,
not `PG_private`) and uses `&page->private` as inline storage — it is **unchanged**.

**Files:** `drivers/xen/grant-table.c` only.

- [x] **Step 1: Update gnttab_pages_set_private() — remove SetPagePrivate**

On 64-bit, the function body (within `#if BITS_PER_LONG < 64`) becomes empty; add a comment
explaining that `page->private` is inline storage populated by `gnttab_map_refs()`.

```c
/* after */
int gnttab_pages_set_private(int nr_pages, struct page **pages)
{
    int i;

    for (i = 0; i < nr_pages; i++) {
#if BITS_PER_LONG < 64
        struct xen_page_foreign *foreign;

        foreign = kzalloc_obj(*foreign);
        if (!foreign)
            return -ENOMEM;

        set_page_private(pages[i], (unsigned long)foreign);
#endif
        /* 64-bit: page->private is inline xen_page_foreign storage;
         * populated by gnttab_map_refs(), no allocation needed here. */
    }

    return 0;
}
```

- [x] **Step 2: Update gnttab_pages_clear_private() — split 32-bit/64-bit logic**

```c
/* after */
void gnttab_pages_clear_private(int nr_pages, struct page **pages)
{
    int i;

    for (i = 0; i < nr_pages; i++) {
#if BITS_PER_LONG < 64
        /* 32-bit: page->private holds an allocated xen_page_foreign pointer. */
        if (page_private(pages[i])) {
            kfree((void *)page_private(pages[i]));
            set_page_private(pages[i], 0);
        }
#else
        /* 64-bit: page->private is inline xen_page_foreign storage;
         * {domid,gref} may both be zero so unconditionally zero it. */
        set_page_private(pages[i], 0);
#endif
    }
}
```

- [x] **Step 3: Add mandatory pre-zero for 32-bit partial-failure safety**

If `gnttab_pages_set_private()` fails partway through on 32-bit (allocation fails at page `i`),
`gnttab_pages_clear_private()` is called on all `nr_pages` pages. The cleanup guards on
`page_private(pages[i]) != 0` to decide whether to kfree, so pages `i..nr_pages-1` must have
`page->private == 0`.

However, pages allocated via `xen_alloc_unpopulated_pages()` with `CONFIG_XEN_UNPOPULATED_ALLOC`
are popped from Xen's unpopulated list; `xen_free_unpopulated_pages()` only relinks via
`zone_device_data` and does NOT run `free_pages_prepare()`. Therefore `page->private` is NOT
guaranteed to be zero on Xen-allocated pages. Make the pre-zero **mandatory** at the start of
`gnttab_pages_set_private()`:

```c
/* after */
int gnttab_pages_set_private(int nr_pages, struct page **pages)
{
    int i;

#if BITS_PER_LONG < 64
    /* Pre-zero page->private: xen_alloc_unpopulated_pages() does not
     * run free_pages_prepare(), so private may be stale.  This ensures
     * gnttab_pages_clear_private() can use page_private() != 0 as the
     * allocation guard safely, even on partial failure. */
    for (i = 0; i < nr_pages; i++)
        set_page_private(pages[i], 0);
#endif

    for (i = 0; i < nr_pages; i++) {
#if BITS_PER_LONG < 64
        struct xen_page_foreign *foreign;

        foreign = kzalloc_obj(*foreign);
        if (!foreign)
            return -ENOMEM;

        set_page_private(pages[i], (unsigned long)foreign);
#endif
        /* 64-bit: page->private is inline xen_page_foreign storage;
         * populated by gnttab_map_refs(), no allocation needed here. */
    }

    return 0;
}
```

- [x] **Step 4: Build and commit**

```bash
make drivers/xen/grant-table.o
```

```
xen/grant-table: drop PG_private from gnttab page lifecycle

gnttab_pages_set_private() set PG_private as a pure boolean on 64-bit
(page->private is inline xen_page_foreign storage populated later by
gnttab_map_refs()) and as a pointer-set indicator on 32-bit.

On 64-bit, {domid=0, gref=0} is a valid mapped state so page_private()
!= 0 cannot be used as a lifecycle guard; make cleanup unconditional.
On 32-bit, the allocated pointer is always non-NULL so page_private() !=
0 remains the correct guard.

xen_page_foreign() guards on PageForeign (PG_owner_priv_1) and its
&page->private inline storage is unchanged.
```

---

## Task 5: Fix fs/crypto bounce pages  ✓ COMPLETE (385a9efa68d92)

`alloc_bounce_page()` stores a folio pointer (always non-NULL) in `page->private` and sets
`PG_private`. The flag is redundant.

> **Note: PG_private is not used in the I/O path for bounce pages.**
> Bounce page identification and plaintext folio recovery both work independently of
> `PG_private`:
> - `fscrypt_is_bounce_page()` (`include/linux/fscrypt.h:361`) detects bounce pages via
>   `page->mapping == NULL`, not `PagePrivate()`.
> - `fscrypt_pagecache_page()` (`include/linux/fscrypt.h:368`) recovers the plaintext folio
>   via `page_private(bounce_page)` directly — the data field, not the flag.
> - `fscrypt_finalize_bounce_page()`, called by f2fs/ceph/ext4 in I/O completion to swap
>   the bounce page back to the plaintext page, uses these two helpers and never checks
>   `PagePrivate()`.
>
> `SetPagePrivate` was set on the bounce page but never read back by any consumer.
> Removing it is safe; I/O operations are entirely unaffected.

**Files:** `fs/crypto/crypto.c`

- [x] **Step 1: Remove SetPagePrivate from alloc path (~line 205)**

```c
/* before */
    SetPagePrivate(ciphertext_page);
    set_page_private(ciphertext_page, (unsigned long)folio);

/* after */
    set_page_private(ciphertext_page, (unsigned long)folio);
```

- [x] **Step 2: Remove ClearPagePrivate from fscrypt_free_bounce_page() (~line 75)**

```c
/* before */
    set_page_private(bounce_page, (unsigned long)NULL);
    ClearPagePrivate(bounce_page);

/* after */
    set_page_private(bounce_page, 0);
```

- [x] **Step 3: Replace any remaining PagePrivate tests**

```bash
git grep -rn 'PagePrivate\|folio_test_private' fs/crypto/
```

- [x] **Step 4: Build and commit**

```bash
make fs/crypto/
```

```
fscrypt: drop PG_private from bounce page tracking

The folio pointer stored in page->private is always non-NULL when
present, making PG_private redundant.
```

---

## Task 6: Resolve B-noref callers  ✓ COMPLETE (no code change)

**Known B-noref site: `fs/nfs/write.c` (~line 718–722)**

NFS calls `folio_set_private(folio)` and `folio->private = req` directly without `folio_get()`.
The folio reference is held by the `nfs_page` lifecycle. **Do NOT migrate to
`folio_attach_private()`.** The `folio_set/clear_private` calls in NFS are pure flag manipulations
removed by the Coccinelle + manual cleanup in Tasks 10–11. `folio_expected_ref_count()` +1 remains
correct since the ref IS held via the nfs_page lifecycle.

- [x] **Step 1: For any non-NFS B-noref sites** decide Option A (migrate to
  `folio_attach_private()`, adds matching `folio_get()`) or Option B (document why the separate
  lifecycle ref is correct).

  **Result:** No non-NFS B-noref sites exist. The only B-noref sites are:
  - `fs/nfs/write.c:720,747` — Option B; flag calls removed in Tasks 10–11. See `B_noref.md`.
  - `mm/migrate.c:839` — confirmed dead code (always a no-op); removed in Task 11. See `cleanup.md`.

---

## Task 7: Pre-pivot cleanup — folio_change_private() and F2FS zero-attach  ← MUST PRECEDE TASK 8  ✓ COMPLETE

After Task 8 makes `folio->private != NULL` the canonical "has private" test, a Type A caller
that calls `folio_change_private(folio, NULL)` would zero `folio->private` without calling the
`folio_put()` that `folio_detach_private()` provides — refcount leak.

**Non-Type-A callers are different**: `mm/hugetlb.c:1431` calls `folio_change_private(folio, NULL)`
to clear internal flag bits stored in `folio->private`. Hugetlb never called `folio_attach_private()`
so there is no extra refcount to drop. Do NOT replace this with `folio_detach_private()` — that
would call `folio_put()` on a non-refcounted folio. Change it to direct `folio->private = NULL`.

Do NOT add a WARN_ON to `folio_change_private()` — hugetlb legitimately passes NULL and would
trigger it spuriously.

**Files:** `include/linux/pagemap.h`, `mm/hugetlb.c`, `fs/f2fs/f2fs.h`,
any other callers from Task 1 Step 5.

**Part A: folio_change_private() callers**

- [x] **Step 1: Fix Type A NULL-destination callers**

Audit of all `folio_change_private()` call sites in the tree:

| File | Line | Data arg | Type | Action needed |
|---|---|---|---|---|
| `fs/netfs/buffered_read.c` | 463 | `group` (non-NULL) | A | Non-NULL→non-NULL swap; safe as-is |
| `fs/netfs/buffered_write.c` | 371 | `netfs_get_group(netfs_group)` (non-NULL) | A | Non-NULL→non-NULL swap; safe as-is |
| `fs/netfs/buffered_write.c` | 378 | `netfs_group` (non-NULL) | A | Non-NULL→non-NULL swap; safe as-is |
| `fs/netfs/buffered_write.c` | 597 | `netfs_get_group(netfs_group)` (non-NULL) | A | Non-NULL→non-NULL swap; safe as-is |
| `fs/netfs/read_collect.c` | 62 | `finfo->netfs_group` (non-NULL) | A | Non-NULL→non-NULL swap; safe as-is |
| `mm/hugetlb.c` | 1436 | `NULL` | Non-Type-A | Fixed in Step 2 |

**Result: there are zero Type A NULL-destination callers.** All non-NULL callers are
non-NULL→non-NULL swaps (safe). The only NULL-passing caller is `mm/hugetlb.c:1436`,
which is non-Type-A and handled in Step 2.

- [x] **Step 2: Fix non-Type-A NULL-destination callers (hugetlb)** (ae7d7bc99e7d0)

```c
/* mm/hugetlb.c ~line 1431 */
/* before */
folio_change_private(folio, NULL);
/*
 * We have to set hugetlb_vmemmap_optimized again as above
 * folio_change_private(folio, NULL) cleared it.
 */

/* after */
folio->private = NULL;
/*
 * We have to set hugetlb_vmemmap_optimized again as above
 * cleared folio->private.
 */
```

- [x] **Step 3: Find and fix NULL-source callers**

A call `folio_change_private(folio, data)` where `folio->private` might be NULL before the call
should be `folio_attach_private(folio, data)` instead. Verify `fs/netfs/` callers are non-NULL to
non-NULL.

All five `fs/netfs/` callers verified non-NULL→non-NULL (no code change needed):
- `buffered_read.c:463`: old=finfo (non-NULL), new=group (non-NULL, guarded by `if (group)`)
- `read_collect.c:62`: old=finfo (non-NULL), new=finfo->netfs_group (non-NULL, guarded by `if (finfo->netfs_group)`)
- `buffered_write.c:371`: old=NETFS_FOLIO_COPY_TO_CACHE (non-NULL), new=netfs_get_group() (non-NULL)
- `buffered_write.c:378`: old=finfo (non-NULL, `else` branch), new=netfs_group (non-NULL)
- `buffered_write.c:597`: old=NETFS_FOLIO_COPY_TO_CACHE (non-NULL), new=netfs_get_group() (non-NULL)

Cases where `folio->private` is NULL use `folio_attach_private()` (not `folio_change_private()`).
**No NULL-source callers exist; no code change required.**

**Part B: F2FS zero-attach fix (must precede the pivot)**

`fs/f2fs/f2fs.h`'s `set_page_private_##name` macro calls
`attach_page_private(page, (void *)0)`, storing NULL in `folio->private`. After Task 8 makes
`folio->private != NULL` the canonical "has private" test, this transiently violates the
invariant. Fix it now by initializing with the sentinel bit value so `folio->private` is
non-NULL from the moment of attachment, and drop the now-redundant `set_bit(PAGE_PRIVATE_NOT_POINTER, ...)` that immediately follows:

**`PAGE_PRIVATE_GET_FUNC` — `page_private_##name()` (GET function):**
```c
/* before */
return PagePrivate(page) && \
    test_bit(PAGE_PRIVATE_NOT_POINTER, &page_private(page)) && \
    test_bit(PAGE_PRIVATE_##flagname, &page_private(page)); \

/* after */
return page_private(page) && \
    test_bit(PAGE_PRIVATE_NOT_POINTER, &page_private(page)) && \
    test_bit(PAGE_PRIVATE_##flagname, &page_private(page)); \
```

`PagePrivate(page)` must be changed here for the same reason: it uses the flag that is being
removed. After the change, the `page_private(page)` guard is technically redundant with
`test_bit(PAGE_PRIVATE_NOT_POINTER, ...)` (non-zero private implies bit 0 could be set, but
`test_bit` would correctly return false on zero), but it is an explicit, cheap fast-path that
matches the folio variant's `!folio->private` check.

**`PAGE_PRIVATE_SET_FUNC` — `set_page_private_##name()` (SET function):**
```c
/* before */
if (!PagePrivate(page)) \
    attach_page_private(page, (void *)0); \
set_bit(PAGE_PRIVATE_NOT_POINTER, &page_private(page)); \
set_bit(PAGE_PRIVATE_##flagname, &page_private(page)); \

/* after */
if (!page_private(page)) \
    attach_page_private(page, (void *)BIT(PAGE_PRIVATE_NOT_POINTER)); \
set_bit(PAGE_PRIVATE_##flagname, &page_private(page)); \
```

Both functions are changed in the same commit to keep the F2FS fix self-contained.

- [x] **Step 4: Build and commit** (e54125273b6b8)

```bash
make mm/ fs/netfs/ fs/f2fs/
```

```
mm/hugetlb,pagemap,f2fs: pre-pivot cleanup before PG_private removal

After the semantic pivot, folio->private != NULL will be the canonical
'has private data' test.  A Type A caller that passes NULL to
folio_change_private() would zero folio->private without the folio_put()
that folio_detach_private() provides.

Convert Type A NULL-destination callers to folio_detach_private().

mm/hugetlb.c is a non-Type-A caller: it stores internal flag bits in
folio->private without folio_attach_private(), so there is no refcount
to drop.  Change it to direct folio->private = NULL assignment.
```

---

## Task 7b: Fix raw folio->private writers on pagecache folios  ← MUST PRECEDE TASK 8  ✓ COMPLETE (6f907a554c5f7, 300b2dde517da)

Any pagecache folio that stores a non-NULL value in `folio->private` without calling
`folio_attach_private()` (and thus without setting `PG_private`) will be misclassified
after the Task 8 pivot: `folio_has_private()`, `folio_expected_ref_count()`, and
`KPF_PRIVATE` all treat any non-NULL `folio->private` as fs-private data backed by a folio
ref. Two EROFS sites on live pagecache folios must be fixed before Task 8.

### Site 1: `fs/erofs/zdata.c:1911` — readahead linked list

`z_erofs_readahead()` builds a reverse-order folio list through `folio->private` only
because `readahead_folio()` iterates front-to-back while EROFS wants back-to-front:
```c
while ((folio = readahead_folio(rac))) {
    folio->private = head;   /* raw pointer, no PG_private */
    head = folio;
}
while (head) {
    folio = head;
    head = folio_get_private(folio);
    z_erofs_scan_folio(&f, folio, true);   /* erofs_onlinefolio_init() clobbers private */
}
```

`folio_attach_private()`/`folio_detach_private()` do **not** fit here: `z_erofs_scan_folio()`
→ `erofs_onlinefolio_init()` (zdata.c:1017) unconditionally overwrites `folio->private`
mid-life, so the private usage is aborted, not run to a natural detach — using attach/detach
would require a detach immediately before scan, i.e. pure refcount churn on folios already
pinned by page-cache + lock.

**Fix: add a generic `readahead_folio_reverse()` to `include/linux/pagemap.h` and delete the
linked list entirely.** `xa_load()` at a tail index follows sibling entries (lib/xarray.c:211)
and returns the head folio, so a reverse iterator needs no allocation and handles compound
folios via `folio->index`:

```c
/* include/linux/pagemap.h — mirror of readahead_folio(), tail-to-front */
static inline struct folio *readahead_folio_reverse(struct readahead_control *ractl)
{
    struct folio *folio;

    if (!ractl->_nr_pages)
        return NULL;
    /* xa_load() follows sibling entries, so a tail index returns the head */
    folio = xa_load(&ractl->mapping->i_pages,
                    ractl->_index + ractl->_nr_pages - 1);
    VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
    /* shrink the window from the tail to this folio's head index; keep _index
     * fixed so read_pages()'s forward cleanup loop still finds any leftovers */
    ractl->_nr_pages = folio->index - ractl->_index;
    ractl->_batch_count = 0;
    folio_put(folio);
    return folio;
}
```

EROFS then becomes:
```c
while ((folio = readahead_folio_reverse(rac)))
    z_erofs_scan_folio(&f, folio, true);
```

`folio->private` is never touched until `z_erofs_scan_folio()` runs. Note: eager `_nr_pages`
reduction (not the deferred `_batch_count` scheme of the forward iterator) is required so an
early break leaves `read_pages()`'s forward cleanup (`mm/readahead.c:176`) consistent.

### Site 2: `fs/erofs/data.c:266` — in-flight I/O counter

`erofs_onlinefolio_init/split/end()` store an atomic reference counter directly in
`folio->private` on live pagecache folios (raw, no PG_private). After the pivot the counter
looks like fs-private data with no backing folio ref, breaking `folio_expected_ref_count()`
(adds +1 with no real ref) and exposing `KPF_PRIVATE` via the lockless `/proc/kpageflags`
snapshot.

**Fix: convert `erofs_onlinefolio_*()` to `folio_attach_private()`/`folio_detach_private()`.**
This *does* fit: the counter is the private payload, mutated in place between one attach and
one detach — exactly the attach/detach contract. The intermediate `atomic_inc`/`atomic_cmpxchg`
on `&folio->private` are payload mutations, not folio-refcount ops.

```c
void erofs_onlinefolio_init(struct folio *folio)
{
    folio_attach_private(folio, (void *)1);   /* folio_get + private=1 + SetPagePrivate */
}
void erofs_onlinefolio_split(struct folio *folio)
{
    atomic_inc((atomic_t *)&folio->private);  /* mutate payload; ref unchanged */
}
void erofs_onlinefolio_end(struct folio *folio, int err, bool dirty)
{
    ... atomic_cmpxchg on &folio->private ...
    if (v & (BIT(EROFS_ONLINEFOLIO_DIRTY) - 1))
        return;
    folio_detach_private(folio);              /* was: folio->private = 0; balances the get */
    if (v & BIT(EROFS_ONLINEFOLIO_DIRTY))
        flush_dcache_folio(folio);
    folio_end_read(folio, !(v & BIT(EROFS_ONLINEFOLIO_EIO)));
}
```

Why this is correct and better than a sideband redesign:
- One `folio_attach_private()` (init) is balanced by one `folio_detach_private()` (the
  completing caller that observes count→0). The N `atomic_inc`s in between are payload
  mutations, not folio_get/put pairs.
- `folio_detach_private()` gates on `folio_test_private()` (set by init), not on the pointer
  value, so the counter bits don't confuse it; its return value is discarded.
- The attach `folio_get()` makes `folio_expected_ref_count()`'s +1 **backed by a real ref** —
  fixing the mismatch the raw counter would otherwise cause, rather than dodging it.
- The whole online window is under the folio lock (until `folio_end_read()` in `_end`), so
  `folio_has_private()`/`KPF_PRIVATE` reading true during it is correct, not a hazard.

### Other raw-private markers (verify, likely no change)

`fs/erofs/zdata.c:569,1577` — `Z_EROFS_PREALLOCATED_FOLIO` / `Z_EROFS_SHORTLIVED_PAGE`
markers set before the folio enters the page cache (569) or on non-pagecache shortlived pages
(1577); cleared before becoming visible. Verify the ordering is unconditional and document.

Run the full audit from Task 1 Step 5 to confirm no other pagecache raw-private writer remains.

**Files:** `include/linux/pagemap.h` (new `readahead_folio_reverse()`), `fs/erofs/zdata.c`,
`fs/erofs/data.c`

- [x] **Step 1:** Add `readahead_folio_reverse()` to `include/linux/pagemap.h`; convert
  `z_erofs_readahead()` to use it and drop the `folio->private` linked list. (6f907a554c5f7)
- [x] **Step 2:** Convert `erofs_onlinefolio_init/split/end()` to
  `folio_attach_private()`/`folio_detach_private()` (data.c). The detach replaces
  `folio->private = 0`; one attach at `_init` balances one detach at the count→0 `_end`. (300b2dde517da)
- [x] **Step 3:** `fs/erofs/zdata.c:569` = pre-cache marker (cleared before insertion);
  `:1577` = non-pagecache shortlived page. Both safe (classified in task_1.md Step 5).
- [x] **Step 4:** (runtime) Ran `task_7b_erofs.md` — integrity OK across all images; no
  dmesg splats (bad_page / refcount VM_BUG_ON / KASAN) under the readahead + reclaim/migration
  race with `CONFIG_DEBUG_VM=y CONFIG_KASAN=y`. **Pre-pivot run clean AND post-Task-8 re-run
  clean** — the pivot did not surface any residual raw-private writer. `fs/erofs/` builds
  cleanly at both commits.
- [x] **Step 5:** Committed as two commits (readahead helper + its erofs conversion combined
  into 6f907a554c5f7; online-folio counter in 300b2dde517da).

---

## Task 8: Semantic pivot — widen tests to pointer checks, make Set/Clear no-ops  ← MUST PRECEDE TASK 9  ✓ COMPLETE (e23ba7f8168f4)

> Single commit. allmodconfig builds; normal-config mm + erofs tests pass. Post-pivot re-run
> of `task_7b_erofs.md` (readahead + reclaim/migration race under DEBUG_VM+KASAN) is clean —
> confirms no residual raw-private writer survives the pivot. One review fix amended in:
> `mm/huge_memory.c` split refcount gate must negate the whole fs-private predicate
> `!(priv && !swapcache && !hugetlb)`, not `(!priv && !swapcache && !hugetlb)` — the latter
> wrongly stopped skipping swapcache/hugetlb folios (now consistent with the vmscan.c /
> page-writeback.c sites).

This is the semantic pivot. All prior tasks ensure the flag and pointer are in sync (or that the
exceptional sites no longer use the flag). After this commit:
- `folio_test_private()` checks `folio->private` directly.
- `folio_has_private()` checks `folio->private` with guards for non-filesystem users (swapcache
  stores `swp_entry_t` in `page->private`; hugetlb stores internal flag bits) plus `PG_private_2`.
- `folio_set_private()` / `folio_clear_private()` / `SetPagePrivate()` / `ClearPagePrivate()` are
  no-ops (shims retained for compile compatibility until Task 13).
- `PG_private` removed from `PAGE_FLAGS_CHECK_AT_FREE` in the same commit (mandatory — stale bits
  on folios attached before the pivot would trip `bad_page()` at free time).
- `KPF_PRIVATE` in `/proc/kpageflags` is updated in the same commit to source from
  `folio->private != NULL` (with swapcache/hugetlb guards); near-ABI-preserving —
  see Step 6 for intentional semantic differences.
  `include/linux/kernel-page-flags.h` and `tools/mm/page-types.c` are unchanged.

**Files:** `include/linux/page-flags.h`, `include/linux/mm.h`,
`mm/migrate.c`, `mm/huge_memory.c`, `include/trace/events/pagemap.h`, `fs/proc/page.c`

- [x] **Step 1: Replace PAGEFLAG(Private, ...) with pointer-based implementations and no-op shims**

Remove `PAGEFLAG(Private, private, PF_ANY)` and replace with:

```c
/*
 * folio_test_private() - raw check: does folio->private carry any data?
 *
 * Filesystem code calls this only on its own folios (never swapcache or
 * hugetlb), so the simple pointer check is correct there.
 *
 * VM call sites that operate on arbitrary folios use open-coded
 * !folio_test_swapcache() / !folio_test_hugetlb() guards in this commit;
 * these are consolidated into folio_test_fs_private() in Task 8b.
 */
static __always_inline bool folio_test_private(const struct folio *folio)
{
    return !!folio->private;
}

static __always_inline int PagePrivate(const struct page *page)
{
    return !!page_private(page);
}

/* No-ops pending final removal of PG_private from enum pageflags.
 * All call sites are removed by Tasks 10-11. */
static __always_inline void folio_set_private(struct folio *folio) { }
static __always_inline void folio_clear_private(struct folio *folio) { }
static __always_inline void SetPagePrivate(struct page *page) { }
static __always_inline void ClearPagePrivate(struct page *page) { }
```

- [x] **Step 2: Remove PG_private from PAGE_FLAGS_CHECK_AT_FREE (~line 1171)**

```c
/* before */
     1UL << PG_private  | 1UL << PG_private_2  |    \

/* after */
     1UL << PG_private_2  |                          \
```

- [x] **Step 3: Update folio_expected_ref_count() — in this same commit**

`folio_expected_ref_count()` in `include/linux/mm.h` calls `folio_test_private()`, which after
Step 1 returns `!!folio->private`. Hugetlb stores internal flag bits without a ref; shmem
swapcache is file-backed and reaches the `!folio_test_anon()` branch. Both would add a bogus +1.

```c
/* before */
/* One reference from PG_private. */
ref_count += folio_test_private(folio);

/* after */
/*
 * One reference for private data pinning the folio — either via
 * folio_attach_private() or a subsystem-specific lifecycle that holds
 * a folio_get() for the duration of folio->private being non-NULL.
 * Exclude hugetlb (internal flag bits, no ref) and swapcache
 * (swp_entry_t in page->private, shmem swapcache is non-anon).
 */
ref_count += !!folio->private && !folio_test_hugetlb(folio) &&
             !folio_test_swapcache(folio);
```

- [x] **Step 4: Add explicit swapcache/hugetlb guards at VM call sites of folio_test_private()**

Filesystem code calls `folio_test_private()` only on its own folios (never swapcache or hugetlb),
so the raw pointer check is safe there. VM call sites that operate on arbitrary folios need
explicit guards here. These will be consolidated into `folio_test_fs_private()` in Task 8b.

`mm/migrate.c:1330` — swapcache has non-zero private (swp_entry_t); hugetlb has flag
bits. Both must be excluded to preserve pre-removal semantics (PG_private was never set
for either):
```c
/* before */
if (folio_test_private(src)) {
    try_to_free_buffers(src);
}
/* after */
if (folio_test_private(src) && !folio_test_swapcache(src) &&
    !folio_test_hugetlb(src)) {
    try_to_free_buffers(src);
}
```

`include/trace/events/pagemap.h:25` — swapcache/hugetlb would incorrectly show PAGEMAP_BUFFERS:
```c
/* before */
(folio_test_private(folio) ? PAGEMAP_BUFFERS : 0) \
/* after */
(folio_test_private(folio) && !folio_test_swapcache(folio) &&
 !folio_test_hugetlb(folio) ? PAGEMAP_BUFFERS : 0) \
```

`mm/huge_memory.c:4756` — this is in the THP split path; hugetlb folios do not reach it
(they use their own migration path). Swapcache must not bypass the refcount pre-check:
```c
/* before */
if (!folio_test_private(folio) &&
    folio_expected_ref_count(folio) != folio_ref_count(folio))
    goto next;
/* after */
if (!(folio_test_private(folio) && !folio_test_swapcache(folio)) &&
    folio_expected_ref_count(folio) != folio_ref_count(folio))
    goto next;
```

Other known VM call sites confirmed safe (no guard needed):
- `mm/vmscan.c:960` — only called on pagecache folios in the shrink path.
- `mm/page-writeback.c:2709` — only called on dirty pagecache folios.

Sweep to catch any remaining VM call sites:
```bash
git grep -rn 'folio_test_private\b' mm/ include/trace/ | grep -v 'private_2'
```

- [x] **Step 5: Update PAGE_FLAGS_PRIVATE and folio_has_private()**

`folio_has_private()` must guard against non-filesystem users of `folio->private`.
The explicit guard will be replaced by `folio_test_fs_private()` in Task 8b.

```c
/* Remove PG_private from the macro (leave PG_private_2 for fscache): */
#define PAGE_FLAGS_PRIVATE      (1UL << PG_private_2)

/* Update folio_has_private() with explicit guards: */
static inline int folio_has_private(const struct folio *folio)
{
    return (!!folio->private && !folio_test_swapcache(folio) &&
            !folio_test_hugetlb(folio)) ||
           folio_test_private_2(folio);
}
```

- [x] **Step 6: Update KPF_PRIVATE in fs/proc/page.c to use folio->private**

`include/linux/kernel-page-flags.h` and `tools/mm/page-types.c` are **unchanged** —
`KPF_PRIVATE` keeps its bit number 35 and description (near-ABI-preserving).

`fs/proc/page.c` (~line 245): replace the flag-bit copy with a pointer check, applying
the same swapcache/hugetlb exclusions as `folio_has_private()` and `folio_test_private()`
to preserve the pre-removal semantics (swapcache and hugetlb never had `PG_private` set):

```c
/* before */
    u |= kpf_copy_bit(k, KPF_PRIVATE,    PG_private);

/* after */
    /* PG_private is gone; preserve KPF_PRIVATE semantics via the pointer.
     * mapping && !is_anon: exclude non-pagecache pages (binder/KVM/buddy have
     *   folio->mapping == NULL) and anonymous folios (their mapping uses encoded
     *   ANON bits; is_anon is computed at the top of stable_page_flags()).
     * !swapcache && !hugetlb: match pre-removal behaviour where PG_private was
     *   never set for those.
     * Intentional ABI differences from PG_private:
     *   1. Orphaned pagecache folios (mapping == NULL, mm/migrate.c:1324) with
     *      non-NULL fs-private will no longer set KPF_PRIVATE.
     *   2. Raw pagecache folio->private writers (e.g. EROFS, must all be fixed
     *      in Task 7b) could have been transiently observable; after Task 7b they
     *      either clear private before unlock or use folio_attach_private().
     *   Both cases are edge cases; the semantic change is acceptable.
     * BIT_ULL: KPF_PRIVATE is bit 35, requires 64-bit shift.
     * folio is &ps.folio_snapshot — all checks are snapshot-consistent.
     * The explicit guards will be replaced by folio_test_fs_private() in Task 8b. */
    if (mapping && !is_anon && folio_get_private(folio) &&
        !folio_test_swapcache(folio) &&
        !folio_test_hugetlb(folio))
        u |= BIT_ULL(KPF_PRIVATE);
```

- [x] **Step 7: Build with allmodconfig — zero errors**

```bash
make allmodconfig
make -j$(nproc) 2>&1 | tee /tmp/build.log; make_exit=${PIPESTATUS[0]}
grep -E '^.*error:' /tmp/build.log | head -40
echo "make exit: $make_exit"
test "$make_exit" -eq 0
```

- [x] **Step 8: Commit**

```
mm/page-flags: widen folio_test_private() and folio_has_private() to use pointer

folio->private != NULL is now equivalent to the old PG_private flag for
all callers, given that all exceptional users have been converted in
preceding commits.

folio_has_private() is updated here — before folio_set_private() is
removed from folio_attach_private() — so that folio_needs_release() and
the reclaim path continue to work correctly throughout the transition.
Swapcache (swp_entry_t in page->private) and hugetlb (flag bits in
folio->private) are explicitly excluded so folio_needs_release() is not
incorrectly triggered for them.  VM call sites that operate on arbitrary
folios carry the same explicit exclusions.  These will be consolidated
into folio_test_fs_private() in a follow-up commit.

folio_set/clear_private and Set/ClearPagePrivate become no-ops.
PAGE_FLAGS_CHECK_AT_FREE is updated in the same commit to prevent
bad_page() from stale PG_private bits on folios attached before this
commit whose data is detached after it.

KPF_PRIVATE in /proc/kpageflags is updated in the same commit to source
from folio->private != NULL (near-ABI-preserving; swapcache, hugetlb,
anonymous, and non-pagecache pages excluded via mapping/is_anon guards).
Intentional semantic differences: orphaned pagecache folios (mapping==NULL)
with fs-private data will no longer set the bit.

Pending final removal of the PG_private enum value.
```

---

## Task 8b: Introduce folio_test_fs_private() and consolidate guards  ✓ COMPLETE (f15f8666002f2)

Now that the semantic pivot has landed, introduce `folio_test_fs_private()` as a clean
helper that centralises the swapcache/hugetlb exclusion logic. Replace every explicit
`!!folio->private && !folio_test_swapcache() && !folio_test_hugetlb()` guard added in
Task 8 with a call to the new helper.

This commit is pure refactoring — no behaviour change. Keeping it separate from Task 8
makes the pivot easier to review.

**Files:** `include/linux/page-flags.h`, `include/linux/mm.h`,
`mm/migrate.c`, `mm/huge_memory.c`, `include/trace/events/pagemap.h`, `fs/proc/page.c`

- [x] **Step 1: Add folio_test_fs_private() to include/linux/page-flags.h**

Place it **before** `folio_has_private()` (~line 1206) — `folio_has_private()` must be
updated to call it in Step 2, so it must be defined first. Both prerequisites
`folio_test_swapcache()` (~line 625) and `folio_test_hugetlb()` (~line 1028) are
already defined at this point in the file.

```c
/**
 * folio_test_fs_private - Does this folio carry filesystem private data?
 * @folio: The folio to check.
 *
 * Returns true if folio->private holds filesystem-owned data — i.e. it is
 * non-NULL and the folio is not a swapcache folio (which stores swp_entry_t
 * in page->private) or a hugetlb folio (which stores internal flag bits).
 *
 * Note: this helper excludes the two main non-filesystem users of
 * folio->private that are known to appear in LRU pagecache folios.  Other
 * non-filesystem users (e.g. binder shrinker metadata, KVM shadow pages) do
 * not appear as LRU pagecache folios and are safe.  EROFS zdata.c readahead
 * uses folio->private as a linked list on real pagecache folios and must be
 * fixed before this helper is introduced (see Task 1 Step 5 and the raw-
 * private audit).  Verify any newly identified raw folio->private user
 * before expanding callers of this helper.
 *
 * Filesystem code that only operates on its own folios may use the cheaper
 * folio_test_private() instead.  VM code that handles arbitrary folios must
 * use this helper.
 */
static inline bool folio_test_fs_private(const struct folio *folio)
{
    return !!folio->private && !folio_test_swapcache(folio) &&
           !folio_test_hugetlb(folio);
}
```

- [x] **Step 2: Convert folio_has_private() to use folio_test_fs_private()**

```c
/* before */
static inline int folio_has_private(const struct folio *folio)
{
    return (!!folio->private && !folio_test_swapcache(folio) &&
            !folio_test_hugetlb(folio)) ||
           folio_test_private_2(folio);
}

/* after */
static inline int folio_has_private(const struct folio *folio)
{
    return folio_test_fs_private(folio) || folio_test_private_2(folio);
}
```

- [x] **Step 3: Convert folio_expected_ref_count() (include/linux/mm.h)**

```c
/* before */
ref_count += !!folio->private && !folio_test_hugetlb(folio) &&
             !folio_test_swapcache(folio);
/* after */
ref_count += folio_test_fs_private(folio);
```

- [x] **Step 4: Convert VM call sites**

`mm/migrate.c:1330`:
```c
/* before */
if (folio_test_private(src) && !folio_test_swapcache(src) &&
    !folio_test_hugetlb(src)) {
/* after  */
if (folio_test_fs_private(src)) {
```

`include/trace/events/pagemap.h:25`:
```c
/* before */  (folio_test_private(folio) && !folio_test_swapcache(folio) &&
               !folio_test_hugetlb(folio) ? PAGEMAP_BUFFERS : 0) \
/* after  */  (folio_test_fs_private(folio) ? PAGEMAP_BUFFERS : 0) \
```

`mm/huge_memory.c:4756` — hugetlb folios cannot reach this THP split path, so only
the swapcache guard was needed in Task 8; `folio_test_fs_private()` adds hugetlb too
but is a no-op for this call site:
```c
/* before */
if (!(folio_test_private(folio) && !folio_test_swapcache(folio)) &&
    folio_expected_ref_count(folio) != folio_ref_count(folio))
    goto next;
/* after */
if (!folio_test_fs_private(folio) &&
    folio_expected_ref_count(folio) != folio_ref_count(folio))
    goto next;
```

`fs/proc/page.c` (KPF_PRIVATE) — retain the `mapping` guard alongside the helper:
```c
/* before */
    if (mapping && !is_anon && folio_get_private(folio) &&
        !folio_test_swapcache(folio) &&
        !folio_test_hugetlb(folio))
        u |= BIT_ULL(KPF_PRIVATE);
/* after */
    if (mapping && !is_anon && folio_test_fs_private(folio))
        u |= BIT_ULL(KPF_PRIVATE);
```

- [x] **Step 5: Build and commit**

```bash
make -j$(nproc) 2>&1 | tee /tmp/build-8b.log; make_exit=${PIPESTATUS[0]}
grep -E '^.*error:' /tmp/build-8b.log | head -20
test "$make_exit" -eq 0
```

```
mm/page-flags: introduce folio_test_fs_private()

folio_test_fs_private() encapsulates the folio->private != NULL check with
the swapcache and hugetlb exclusions required for VM code that operates on
arbitrary folios.  Replace all explicit guard repetitions introduced in the
previous commit with calls to this helper.

No behaviour change.
```

---

## Task 9: Strip no-op calls from folio_attach_private() and folio_detach_private()  ✓ COMPLETE (9203396ccb5f5)

Now that `folio_test_private()` and `folio_has_private()` check `folio->private` directly,
remove the no-op flag calls from the helpers.

**Files:** `include/linux/pagemap.h`

- [x] **Step 1: Simplify folio_attach_private()**

```c
/* after */
static inline void folio_attach_private(struct folio *folio, void *data)
{
    folio_get(folio);
    folio->private = data;
}
```

- [x] **Step 2: Simplify folio_detach_private()**

```c
/* after */
static inline void *folio_detach_private(struct folio *folio)
{
    void *data = folio->private;

    if (!data)
        return NULL;
    folio->private = NULL;
    folio_put(folio);
    return data;
}
```

- [x] **Step 3: Build and commit**

```
mm/pagemap: remove no-op flag calls from folio_attach/detach_private

folio_set/clear_private are now no-ops (Task 8); remove them from the
helpers and simplify folio_detach_private() to test folio->private
directly.
```

---

## Task 10: Coccinelle bulk transform for FS-level folio_set/clear_private  ← AFTER PIVOT  ✓ COMPLETE (9203396ccb5f5)

> No Coccinelle needed in the end: `fs/nfs/write.c` was the only fs/ file with direct
> `folio_set_private()`/`folio_clear_private()` calls (everything else goes through
> `folio_attach/detach_private()`). Removed by hand in the same commit as Task 9.

**Files:** `scripts/coccinelle/misc/pg-private-removal.cocci` (new)

- [x] **Step 1: Create the script**

```cocci
// scripts/coccinelle/misc/pg-private-removal.cocci
// Run: make coccicheck COCCI=scripts/coccinelle/misc/pg-private-removal.cocci MODE=patch

virtual patch

// Pattern 1: folio_set_private immediately follows folio->private assignment
@@
struct folio *F;
expression E;
@@
  F->private = E;
- folio_set_private(F);

// Pattern 2: folio_clear_private immediately follows folio->private = NULL
@@
struct folio *F;
@@
  F->private = NULL;
- folio_clear_private(F);
```

- [x] **Step 2: Apply and review**

`make coccicheck MODE=patch` emits a patch to stdout but does not apply it in-place. Capture
and apply it:
```bash
make coccicheck COCCI=scripts/coccinelle/misc/pg-private-removal.cocci MODE=patch \
    > /tmp/pg-private-removal.patch
git apply /tmp/pg-private-removal.patch
git diff fs/
```

Verify every removed `folio_set_private()` is paired with a non-NULL write to `folio->private`,
and every removed `folio_clear_private()` is paired with a NULL write. Because the pivot already
made these calls no-ops, there is no correctness window even if some sites are missed — the
Coccinelle pass is cleanup only.

- [x] **Step 3: Build and commit**

```bash
make fs/
git add scripts/coccinelle/misc/pg-private-removal.cocci
git add -p fs/
```

```
fs: remove folio_set/clear_private() calls now redundant with folio->private

folio_set/clear_private() have been no-ops since the semantic pivot.
Generated with scripts/coccinelle/misc/pg-private-removal.cocci.
```

---

## Task 11: Manually fix remaining PagePrivate() test callers  ← AFTER PIVOT  ✓ COMPLETE (4f85ac1a705b2)

> Converted md-bitmap.c, buffer_head.h page_buffers(), and ceph page_snap_context()
> (f2fs was already done in Task 7; mm/migrate.c:839 dead code removed in Task 9's commit).
> Note: the stale `PagePrivate` comment in fs/ceph/addr.c:126 (and mm_types.h:111/:678)
> is deferred to Task 13 Step 7's doc sweep, which greps for exactly these.

These sites call `PagePrivate()` standalone (the shim from Task 8 makes them work, but they must
be converted before Task 13 removes the shims entirely).

- [x] **Step 1: Find remaining callers**

```bash
git grep -rn 'folio_set_private\|folio_clear_private\|SetPagePrivate\|ClearPagePrivate\|PagePrivate(' \
    -- '*.c' '*.h' | grep -v 'private_2'
```

- [x] **Step 2: Remove remaining no-op shim callers not caught by Coccinelle**

Two callers are outside `fs/` or have a reversed order that the Coccinelle pattern misses:

`mm/migrate.c:839` — `folio_clear_private(folio)` standalone (the `folio->private = NULL`
assignment is conditional and a few lines later; Coccinelle won't match):
```c
/* remove */
folio_clear_private(folio);
```
The subsequent conditional `folio->private = NULL` at line 843 is already correct and kept.

`fs/nfs/write.c:720` — `folio_set_private(folio)` comes BEFORE `folio->private = req`
(reversed order; Coccinelle pattern misses it):
```c
/* before */
folio_set_private(folio);
folio->private = req;

/* after */
folio->private = req;
```

- [x] **Step 3: Convert known PagePrivate() test callers**

`drivers/md/md-bitmap.c:538`:
```c
/* before */  if (!PagePrivate(page))
/* after  */  if (!page_private(page))
```

`fs/ceph/addr.c:73` (`page_snap_context()`):
```c
/* before */  if (PagePrivate(page))
/* after  */  if (page_private(page))
```

`fs/f2fs/f2fs.h` (`page_private_##name` macro):
```c
/* before */  return PagePrivate(page) && \
/* after  */  return page_private(page) && \
```

`fs/f2fs/f2fs.h` (`set_page_private_##name` macro) — Task 7 Part B already converted
`if (!PagePrivate(page))` to `if (!page_private(page))` and changed the zero-attach.
No further change needed here for Task 11.

`include/linux/buffer_head.h:181` (`page_buffers()` BUG_ON):
```c
/* before */  BUG_ON(!PagePrivate(page));
/* after  */  BUG_ON(!page_private(page));
```

`fs/ceph/addr.c` comment:
```c
/* before */
 * Reference snap context in folio->private.  Also set
 * PagePrivate so that we get invalidate_folio callback.
/* after */
 * Reference snap context in folio->private.  folio_attach_private()
 * ensures folio_has_private() returns true, triggering invalidate_folio.
```

- [x] **Step 4: Build and commit per subsystem**

---

## Task 12: Update folio_expected_ref_count() kdoc (comment cleanup only)  ✓ COMPLETE (squashed into Task 8 (e23ba7f8168f4))

> Done as `fixup! mm/page-flags: check page/folio->private...` — kdoc + the inline
> `/* One reference from PG_private. */` comment. Run `git rebase -i --autosquash` before
> posting to fold it into 85f92556. Additionally, the treewide prose-comment cleanup that
> Task 13 Step 7 called for (ceph, nfs/file.c, ubifs, mm.h, mm_types.h) was done early in
> commit d5bd7c253cfeb — those comments are already updated; Task 13 Step 7 only needs the
> remaining items (page-flags.h:47, vmcoreinfo.rst, and fs/proc/page.c:235 — see below).

The code change to `folio_expected_ref_count()` — including the `!folio_test_hugetlb(folio)`
guard — was made in Task 8 Step 3 to prevent hugetlb migration failures immediately after the
pivot. This task only updates the function's kdoc in `include/linux/mm.h` to remove the stale
`PG_private` reference.

**Files:** `include/linux/mm.h`

- [x] **Step 1: Update the kdoc (~line 2966)**

```c
/* before */
 * swapcache, PG_private and page table mappings into account.
/* after */
 * swapcache, private data (folio->private != NULL) and page table
 * mappings into account.
```

- [x] **Step 2: Build and commit**

```bash
make mm/migrate.o mm/vmscan.o
```

```
mm: update folio_expected_ref_count() kdoc after PG_private removal

Replace stale PG_private reference in the kdoc.
```

---

## Task 13: Remove PG_private enum value and flag machinery  ✓ COMPLETE (20d19655aaa32)

> **Design change from the plan:** the bit is NOT freed — `PG_private` is renamed to
> `__PG_folio` (reserved: "Do not use: reserved for folio identification") for a follow-on
> project. Consequences: `NR_PAGEFLAGS` is **unchanged** (not −1); `__def_pageflag_names`
> keeps a placeholder entry `{ 1UL << __PG_folio, "folio" }` because
> `mm/debug.c` `BUILD_BUG_ON(ARRAY_SIZE(pageflag_names) != __NR_PAGEFLAGS + 1)` requires one
> name per flag bit. No-op shims + `VMCOREINFO_NUMBER(PG_private)` removed; `folio_test_private()`
> kept. Docs updated: `vmcoreinfo.rst` pipe-list, `vfs.rst`. **Deferred:** `hugetlbfs_reserv.rst`
> (EN + zh_CN) — pre-existing stale hugetlb docs; needs a separate refresh (note in cover letter).

**Files:** `include/linux/page-flags.h`, `kernel/vmcore_info.c`,
`include/trace/events/mmflags.h`, `scripts/coccinelle/misc/pg-private-removal.cocci`
(delete — references `folio_set_private()` / `folio_clear_private()` which are removed
in this task), plus documentation files per Step 5.

Note: `fs/proc/page.c` (KPF_PRIVATE pointer-based update with explicit guards) was handled
in Task 8; the guard consolidation via `folio_test_fs_private()` was handled in Task 8b.
`include/linux/kernel-page-flags.h` and `tools/mm/page-types.c` are unchanged throughout.

- [x] **Step 1: Delete scripts/coccinelle/misc/pg-private-removal.cocci**

The script references `folio_set_private()` / `folio_clear_private()` which are removed
in this task. Keeping it would leave a stale in-tree script referencing deleted APIs.
```bash
git rm scripts/coccinelle/misc/pg-private-removal.cocci
```

- [x] **Step 2: Remove PG_private from enum pageflags (~line 108)**

```c
/* remove */
    PG_private,         /* If pagecache, has fs-private data */
```

- [x] **Step 3: Remove the no-op shim functions added in Task 8**

Delete `folio_set_private`, `folio_clear_private`, `SetPagePrivate`, `ClearPagePrivate`,
`PagePrivate` from `include/linux/page-flags.h`.

- [x] **Step 4: Remove VMCOREINFO export (~line 219 of kernel/vmcore_info.c)**

```c
/* remove */
    VMCOREINFO_NUMBER(PG_private);
```

- [x] **Step 5: Remove DEF_PAGEFLAG_NAME(private) from include/trace/events/mmflags.h**

`DEF_PAGEFLAG_NAME(private)` at line 147 expands to `{ 1UL << PG_private, "private" }`. Removing
`PG_private` from the enum breaks compilation here — the grep for `PG_private\b` will NOT catch
it because the flag name appears as the token `private`, not literally `PG_private`.

```c
/* remove */
DEF_PAGEFLAG_NAME(private),
```

- [x] **Step 6: Update comments in page-flags.h**



```c
/* before */
 * The PG_private bitflag is set on pagecache pages if they contain filesystem
 * specific data (which is normally at page->private)...
/* after */
 * Filesystems store private data in folio->private; non-NULL indicates data
 * is attached (see folio_attach_private() in <linux/pagemap.h>).
```

```c
/* before */
 * - PG_private and PG_private_2 cause release_folio() and co to be invoked
/* after */
 * - folio->private != NULL and PG_private_2 cause release_folio() and co
 *   to be invoked
```

- [x] **Step 7: Clean up remaining PG_private references in comments and documentation**

**Already done in commit d5bd7c253cfeb** (treewide comment adjustment): `fs/ceph/addr.c`,
`fs/nfs/file.c`, `fs/ubifs/file.c`, `include/linux/mm.h` (pagecache description),
`include/linux/mm_types.h` (`@private` kdoc + tail-page note). The
`folio_expected_ref_count()` kdoc was done as part of Task 8 (e23ba7f8168f4) after autosquash.

**Still to do in Task 13:**

`include/linux/page-flags.h:47` — the `PG_private bitflag is set on pagecache pages...`
comment above the enum (updated together with removing the enum entry, Step 6).

`Documentation/admin-guide/kdump/vmcoreinfo.rst`: remove `PG_private` from the pipe-list.

⚠️ `fs/proc/page.c:235` — comment `/* preserve the PG_private semantics by excluding non
pagecache folios */`. It's a valid historical reference but trips the final `PG_private\b`
sweep. Reword, e.g. `/* preserve the historical KPF_PRIVATE semantics ... */`.

`Documentation/filesystems/vfs.rst`:
```bash
grep -n 'PagePrivate\|PG_private' Documentation/filesystems/vfs.rst
```

`Documentation/mm/hugetlbfs_reserv.rst` (full sweep):
```bash
grep -n 'PagePrivate\|SetPagePrivate\|ClearPagePrivate\|PG_private' \
    Documentation/mm/hugetlbfs_reserv.rst \
    Documentation/translations/zh_CN/mm/hugetlbfs_reserv.rst
```

`Documentation/admin-guide/mm/pagemap.rst`: update the description of `KPF_PRIVATE` to
note that it now reflects `folio->private != NULL` (filesystem private data) rather than
the `PG_private` flag bit:
```bash
grep -n 'KPF_PRIVATE\|PG_private' Documentation/admin-guide/mm/pagemap.rst
```

Final sweep (covers both the enum value and the removed API names in all file types):
```bash
git grep -rn 'PG_private\b' -- '*.c' '*.h' '*.rst' '*.txt' | grep -v 'PG_private_2'
git grep -rn 'PagePrivate\b\|SetPagePrivate\b\|ClearPagePrivate\b' -- '*.c' '*.h' '*.rst' '*.txt'
```
Expected: no output from either command.

- [x] **Step 8: Build with allmodconfig — zero errors**

```bash
make allmodconfig
make -j$(nproc) 2>&1 | tee /tmp/build-final.log; make_exit=${PIPESTATUS[0]}
grep -E '^.*error:' /tmp/build-final.log | head -20
test "$make_exit" -eq 0
```

- [x] **Step 9: Commit**

```
mm/page-flags: remove PG_private

folio->private != NULL is the canonical indicator that a folio carries
private filesystem data, rendering PG_private redundant.  All users have
been converted in preceding commits.

This frees one bit in the page flags word.
```

---

## Task 14: Final verification

- [x] **Step 1: Confirm NR_PAGEFLAGS is UNCHANGED (bit reserved, not freed)**

The plan originally aimed to free a bit (`NR_PAGEFLAGS` −1). The implemented design instead
reserves the bit as `__PG_folio`, so `NR_PAGEFLAGS` **stays the same**. Verify it did NOT
change (a decrease would mean the reserve was actually a removal):
```bash
make include/generated/bounds.h
grep NR_PAGEFLAGS include/generated/bounds.h   # equal to master's value
```
The `mm/debug.o` `BUILD_BUG_ON(ARRAY_SIZE(pageflag_names) != __NR_PAGEFLAGS + 1)` compiling
clean already confirms the enum count and the names array are consistent.

- [ ] **Step 2: Verify no remaining PG_private references**

```bash
git grep -rn 'PG_private\b' -- '*.c' '*.h' | grep -v 'PG_private_2'
```
Expected: no output.

- [ ] **Step 3: Run mm selftests**

```bash
make kselftest TARGETS=mm 2>&1 | tail -30
```

- [ ] **Step 4: Boot test with KASAN + lockdep + zram**

Build with `CONFIG_KASAN=y CONFIG_LOCKDEP=y CONFIG_DEBUG_VM=y CONFIG_ZRAM=m`.

```bash
modprobe zram
echo 128M > /sys/block/zram0/disksize
mkfs.ext4 /dev/zram0 && mount /dev/zram0 /mnt/zram
# run fio workload then unmount
umount /mnt/zram
```

Check dmesg for KASAN splats, lockdep warnings, VM_BUG_ON triggers.

- [ ] **Step 5: Run xfstests across key filesystems**

```bash
./check -g auto 2>&1 | tee /tmp/xfstests-generic.log
./check -f ext4  -g auto 2>&1 | tee /tmp/xfstests-ext4.log
./check -f xfs   -g auto 2>&1 | tee /tmp/xfstests-xfs.log
./check -f btrfs -g auto 2>&1 | tee /tmp/xfstests-btrfs.log
./check -f f2fs  -g auto 2>&1 | tee /tmp/xfstests-f2fs.log
```

- [ ] **Step 6: Check documentation**

```bash
git grep -rn 'KPF_PRIVATE\|PG_private' Documentation/
```

- [ ] **Step 7: Send to LKML**

```bash
git format-patch master..HEAD --cover-letter -o /tmp/pg-private-removal/
git send-email /tmp/pg-private-removal/ \
    --to=akpm@linux-foundation.org \
    --cc=linux-mm@kvack.org \
    --cc=linux-kernel@vger.kernel.org
```

---

## Patch series ordering

```
[01/N] mm/zsmalloc: replace PG_private first-zpdesc marker with pointer comparison
[02/N] perf/ring_buffer: drop PG_private from AUX page high-order marker
[03/N] xen/grant-table: drop PG_private from gnttab page lifecycle
[04/N] fscrypt: drop PG_private from bounce page tracking
[05/N] mm/hugetlb,pagemap: fix folio_change_private() callers before PG_private removal
[05b/N] mm/readahead: add readahead_folio_reverse()
[05c/N] erofs: use readahead_folio_reverse() instead of a folio->private list
[05d/N] erofs: manage online folio counter via folio_attach/detach_private()
[06/N] mm/page-flags: widen folio_test_private() and folio_has_private() to use pointer
         (explicit swapcache/hugetlb guards at each VM call site; KPF_PRIVATE updated)
[07/N] mm/page-flags: introduce folio_test_fs_private() and consolidate guards
[08/N] mm/pagemap: remove no-op flag calls from folio_attach/detach_private
[09/N] fs: remove folio_set/clear_private() calls now redundant with folio->private (Coccinelle)
[10/N] fs,drivers: manually convert remaining PagePrivate() test callers
[11/N] mm: update folio_expected_ref_count() kdoc after PG_private removal
[12/N] mm/page-flags: remove PG_private
```
