# Standalone Cleanup: Remove dead folio_clear_private() from folio_migrate_flags()

## Summary

`folio_clear_private(folio)` at `mm/migrate.c:839` (inside `folio_migrate_flags()`) is a
no-op in every call path in the current tree. It should be removed as a standalone cleanup,
independent of the broader PG_private removal series.

---

## The dead code

```c
/* mm/migrate.c, inside folio_migrate_flags() */
	if (folio_test_swapcache(folio))
		folio_clear_swapcache(folio);
	folio_clear_private(folio);             /* ← dead: always a no-op */

	/* page->private contains hugetlb specific flags */
	if (!folio_test_hugetlb(folio))
		folio->private = NULL;
```

---

## Why it is dead: all four callers of folio_migrate_flags()

`folio_migrate_flags()` is exported (`EXPORT_SYMBOL`) and has exactly four in-tree callers.
In every path, `PG_private` is already clear on the source folio before
`folio_migrate_flags()` is called.

### Caller 1: `mm/migrate.c:892` — `__migrate_folio()`

```c
static int __migrate_folio(struct address_space *mapping, struct folio *dst,
			   struct folio *src, void *src_private,
			   enum migrate_mode mode)
{
	...
	if (src_private)
		folio_attach_private(dst, folio_detach_private(src));  /* (A) */

	folio_migrate_flags(dst, src);                              /* (B) */
	return 0;
}
```

- **`src_private != NULL` path** (e.g. `filemap_migrate_folio()`, which passes
  `folio_get_private(src)`): `folio_detach_private(src)` at (A) clears `PG_private` and sets
  `src->private = NULL` before (B) runs. `folio_clear_private()` inside (B) clears an already-
  clear flag.

- **`src_private == NULL` path** (`migrate_folio()`, used for folios without private data):
  `PG_private` is guaranteed clear when `migrate_folio()` is reached because:
  - `migrate_folio()`'s own comment: *"suitable for folios that do not have private data"*
  - `fallback_migrate_folio()` calls `filemap_release_folio(src, GFP_KERNEL)` first
    (`mm/migrate.c:1088`), which invokes `a_ops->release_folio()` and clears private state
    before calling `migrate_folio()`. If release fails, `migrate_folio()` is not called.
  - Swapcache folios use `page->private` for the swap entry under `PG_swapcache`,
    not `PG_private`. Shmem folios (which use `shmem_aops` and reach `migrate_folio()`)
    are page-cache folios; shmem does not attach `PG_private` to them.
  - Filesystems with actual private data either provide their own `migrate_folio` op
    (btrfs, f2fs, etc.) or go through `filemap_migrate_folio()` which passes the private
    pointer explicitly, taking the `src_private != NULL` path above.

`__migrate_folio()` is the only path in `mm/migrate.c` that calls `folio_migrate_flags()`.
All migrate_folio/filemap_migrate_folio/buffer_migrate_folio call chains funnel through it.

### Caller 2: `fs/aio.c:513` — `aio_migrate_folio()`

AIO ring-buffer folios are page-cache folios in an anonymous inode mapping (allocated via
`__filemap_get_folio()` at `fs/aio.c:577`). The AIO subsystem does not attach private data
to these folios; `PG_private` is never set on them. `folio_clear_private()` is always a
no-op here.

### Caller 3: `fs/hugetlbfs/inode.c:1040` — `hugetlbfs_migrate_folio()`

Hugetlb stores internal flag bits in `folio->private` but never sets `PG_private`. The
comment at `mm/migrate.c:841` acknowledges this: *"page->private contains hugetlb specific
flags"*. `folio_clear_private()` is always a no-op on hugetlb folios.

### Caller 4: `mm/migrate_device.c:1239` — device migration

Device-private pages are anonymous (no pagecache, no filesystem private data). `PG_private`
is never set. `folio_clear_private()` is always a no-op here.

---

## How it became dead code

**2006, b20a35035f983 — Christoph Lameter, "page migration reorg":**
`ClearPagePrivate(page)` was added as part of the teardown sequence in
`migrate_page_states()`:

```c
ClearPageSwapCache(page);
ClearPageActive(page);
ClearPagePrivate(page);    /* ← original introduction */
set_page_private(page, 0);
page->mapping = NULL;
```

At this point there was no `detach_page_private()` API. The buffer-head migration in
`__buffer_migrate_page()` manually did `ClearPagePrivate(page)` + `set_page_private(page, 0)`
before calling `migrate_page_copy()`. The `ClearPagePrivate()` in `migrate_page_states()`
was a generic catch-all teardown, not the sole mechanism; it covered non-buffer-head private
users that had no per-type clear of their own.

**2020, cd0f371544438 — Guoqing Jiang / akpm, "mm/migrate.c: call detach_page_private to cleanup code":**
`__buffer_migrate_page()` was refactored to use `attach_page_private(newpage,
detach_page_private(page))`. `detach_page_private()` clears `PG_private` and zeroes
`page->private` in sequence before dropping the folio reference. After this, the generic
`ClearPagePrivate()` in `migrate_page_states()` was redundant for the buffer-head path;
it remained as an unchanged carry-over in the common teardown code.

**2021, 19138349ed59b9 — Matthew Wilcox, "mm/migrate: Add folio_migrate_flags()":**
`migrate_page_states()` was mechanically converted to `folio_migrate_flags()`.
`ClearPagePrivate(page)` became `folio_clear_private(folio)`. The call was never
re-examined for redundancy.

**2022, 2ec810d59602f — Matthew Wilcox, "mm/migrate: Add filemap_migrate_folio()":**
`filemap_migrate_folio()` was introduced as the canonical migration path for filesystems
with private data. It explicitly passes `folio_get_private(src)` into the internal
migration function, which then calls `folio_detach_private(src)` when non-NULL —
clearing `PG_private` on the source folio before `folio_migrate_flags()` runs.

**2024, 940d6683c7995 — Kefeng Wang, "mm: migrate: remove migrate_folio_extra()":**
`migrate_folio_extra()` was folded into `__migrate_folio()` with a `src_private`
parameter shared by both `migrate_folio()` (NULL) and `filemap_migrate_folio()`
(non-NULL). This made the `folio_detach_private(src)` call the canonical pre-flag-
migration step in all private-data paths, completing the redundancy of
`folio_clear_private()` in `folio_migrate_flags()`.

The safety net that `ClearPagePrivate()` once provided has been structurally superseded
in every call path. The call was never removed.

---

## Proposed change

**File:** `mm/migrate.c`

```c
/* before */
	if (folio_test_swapcache(folio))
		folio_clear_swapcache(folio);
	folio_clear_private(folio);

	/* page->private contains hugetlb specific flags */
	if (!folio_test_hugetlb(folio))
		folio->private = NULL;

/* after */
	if (folio_test_swapcache(folio))
		folio_clear_swapcache(folio);

	/* page->private contains hugetlb specific flags */
	if (!folio_test_hugetlb(folio))
		folio->private = NULL;
```

---

## Safety argument

The removal is safe because:

1. **`__migrate_folio()` — `src_private != NULL` path (filemap/buffer private-data migration):**
   `folio_detach_private(src)` at line 890 clears `PG_private` and zeros `src->private`
   before `folio_migrate_flags()` is called. Removing the redundant clear changes nothing
   observable.

2. **`src_private == NULL` path (`migrate_folio()`):** `PG_private` is clear by the time
   `migrate_folio()` is reached — callers either operate on folios with no private state,
   or clear/flush it first (e.g. `fallback_migrate_folio()` calls `filemap_release_folio()`
   beforehand; NFS flushes/waits; btrfs btree releases private state at `fs/btrfs/disk-io.c:474`).
   Removing the no-op flag clear changes nothing.

3. **AIO, hugetlb, migrate_device callers:** `PG_private` is never set on these folio types.
   The call is unconditionally a no-op.

4. **`folio->private = NULL` at the following line (line 843) is preserved:** This correctly
   zeroes the private data field for non-hugetlb folios. It is independent of the flag and
   remains correct.

5. **`PAGE_FLAGS_CHECK_AT_FREE`:** `PG_private` is in the free-check mask. If the flag were
   still set when the source folio is freed, `bad_page()` would fire. Since `folio_detach_private()`
   already clears it before `folio_migrate_flags()`, this invariant is upheld regardless of
   whether the redundant `folio_clear_private()` is present.

---

## Suggested commit message

```
mm/migrate: remove redundant folio_clear_private() from folio_migrate_flags()

folio_clear_private() in folio_migrate_flags() is a no-op in every call
path:

- __migrate_folio() with src_private != NULL: folio_detach_private(src) at
  line 890 already clears PG_private and zeroes src->private before
  folio_migrate_flags() runs. This covers all filemap_migrate_folio() and
  buffer_migrate_folio() paths (private-data callers).

- migrate_folio() (src_private == NULL): PG_private is clear by the time
  migrate_folio() is called. fallback_migrate_folio() calls
  filemap_release_folio() first; filesystems with private data either
  provide their own migrate_folio op or go through filemap_migrate_folio();
  swapcache uses PG_swapcache (not PG_private) for the swap entry; shmem
  folios that reach migrate_folio() do not attach PG_private.

- aio_migrate_folio(): AIO ring folios are page-cache folios in an anonymous
  inode mapping; AIO does not attach PG_private to them.

- hugetlbfs_migrate_folio(): hugetlb stores flag bits in folio->private but
  never sets PG_private.

- __migrate_device_pages(): device-private pages carry no filesystem private
  data; PG_private is not set.

The call originates from ClearPagePrivate() added in b20a35035f983 ("page
migration reorg", 2006), when there was no detach_page_private() API.
It was superseded incrementally:
  cd0f371544438 ("mm/migrate.c: call detach_page_private to cleanup code",
    2020) made it redundant for the buffer-head path but left the common
    teardown clear in place.
  2ec810d59602f ("mm/migrate: Add filemap_migrate_folio()", 2022) introduced
    the explicit src_private path that calls folio_detach_private() before
    folio_migrate_flags().
  940d6683c7995 ("mm: migrate: remove migrate_folio_extra()", 2024) made
    folio_detach_private() the canonical pre-flag-migration step for
    private-data migration paths, completing the redundancy.

The folio->private = NULL assignment that follows is preserved: it zeroes
the private data field for non-hugetlb folios and is independent of the flag.

Signed-off-by: <author>
```
