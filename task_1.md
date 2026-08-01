# Task 1: Full Audit of PG_private Call Sites

**Total lines in audit output:** 150 (from the git grep command in Step 1 below; the exact count
depends on the grep pattern — it excludes `private_2` variants but includes comments and API
docs within matched files. Use the Step 1 command to reproduce exactly.)
**Tree:** linux-remove-PG_private (HEAD 56b9d68cf62f5)

---

## Step 1: Flag-accessor and API caller count

Exact command used (results saved to pg_private_audit.txt):
```bash
git grep -rn \
  -e 'SetPagePrivate\|ClearPagePrivate\|PagePrivate(' \
  -e '__SetPagePrivate\|__ClearPagePrivate' \
  -e 'folio_set_private\|folio_clear_private\|folio_test_private' \
  -e 'folio_attach_private\|folio_detach_private' \
  -e 'attach_page_private\|detach_page_private' \
  -- '*.c' '*.h' \
  | grep -v 'private_2' \
  | sort > pg_private_audit.txt
# Result: 150 lines
```

Note: this pattern matches accessor call sites and comments within matched files. The
`PAGEFLAG(Private, private, PF_ANY)` macro invocation at `include/linux/page-flags.h:581`
generates all PagePrivate/folio_set_private/etc symbols but does not itself match any of the
patterns above. It is listed separately in the non-call-site dependencies section.

---

## Step 2: Networking and driver sweep results

| Subsystem | Result |
|---|---|
| `net/core/page_pool.c` | Clean — uses `pp_magic` / page-pool metadata fields (see `include/linux/mm_types.h:118`, `include/linux/mm.h:5371`), not `PagePrivate` |
| `drivers/gpu/drm/` | Clean — no PagePrivate usage |
| `drivers/perf/` | Clean |
| `drivers/hwtracing/` | Clean |
| `arch/arm64/kernel/` (perf files) | Clean — path does not contain `PagePrivate` references |
| `arch/x86/events/intel/bts.c` | 1 site — `PagePrivate(page)` at line 69 |
| `arch/x86/events/intel/pt.c` | 2 sites — `PagePrivate(p)` at lines 777, 1292 |
| `kernel/events/ring_buffer.c` | 2 sites — `SetPagePrivate(page)`:637, `ClearPagePrivate(page)`:648 |

---

## Step 3: Categorised site list

### Type A — folio_attach_private() / folio_detach_private() (standard, refcounted)

These sites use the correct attach/detach API. No code change needed — they gain correct
behaviour automatically when Task 9 simplifies those helpers.

| File | Lines | Notes |
|---|---|---|
| `fs/btrfs/extent_io.c` | 910, 924, 945 (attach); 973, 2934, 3315 (detach) | eb / prealloc / EXTENT_FOLIO_PRIVATE |
| `fs/btrfs/subpage.c` | 66 (attach); 83 (detach) | bfs (btrfs_folio_state) |
| `fs/btrfs/disk-io.c` | 527 (detach) | cleanup on error |
| `fs/buffer.c` | 897, 1689 (attach); 2844 (detach) | buffer_head list |
| `fs/ceph/addr.c` | 131 (attach); 161, 863 (detach); 949 (detach_page_private) | snap_context |
| `fs/erofs/zdata.c` | 1540, 1580 (attach); 610, 634 (detach) | pcl (pcluster) |
| `fs/f2fs/compress.c` | 92 (attach); 598 (detach) | compress page data |
| `fs/f2fs/data.c` | 2507 (attach); 2516, 2520 (detach) | f2fs_folio_state |
| `fs/f2fs/dir.c` | 922 (detach) | |
| `fs/f2fs/f2fs.h` | 2657 (folio_attach in macro), 2678 (folio_detach in macro), 2686 (detach_page_private in macro), 2718 (folio_attach) | PAGE_PRIVATE macros |
| `fs/iomap/buffered-io.c` | 251 (attach); 258 (detach) | iomap_folio_state |
| `fs/jfs/jfs_metapage.c` | 104, 216 (attach); 131, 224 (detach) | metapage |
| `fs/netfs/buffered_read.c` | 465 (detach) | |
| `fs/netfs/buffered_write.c` | 321, 373, 599 (attach); 369, 380, 595 (detach) | netfs_group / netfs_folio_info |
| `fs/netfs/misc.c` | 288 (detach) | |
| `fs/netfs/read_collect.c` | 64 (detach); 71 (attach — NETFS_FOLIO_COPY_TO_CACHE, non-NULL constant) | |
| `fs/netfs/write_collect.c` | 78, 89, 99 (detach) | |
| `fs/netfs/write_issue.c` | 55 (detach) | |
| `fs/orangefs/inode.c` | 357, 674 (attach); 62, 115, 405, 410, 475 (detach) | orangefs_write_range |
| `fs/ubifs/file.c` | 559, 1551 (attach — `(void *)1`, non-NULL integer); 928, 1301, 1480 (detach) | |
| `drivers/md/md-bitmap.c` | 575 (attach_page_private); 547 (detach_page_private) | buffer_head |
| `mm/migrate.c` | 890 (folio_attach_private(dst, folio_detach_private(src))) | migrate private data |

**folio_test_private() calls in Type A context** (test that own private data is present — safe):
btrfs/extent_io.c:548, 909, 917, 937, 966, 1240, 1916, 2895, 2916, 2929, 3301, 4574, 4594;
btrfs/subpage.c:55, 76, 127, 141, 153, 698, 750; btrfs/file.c:819; btrfs/inode.c:7426;
fs/ceph/addr.c:93 (VM_BUG_ON_FOLIO assert), 157 (owns folio, Type A context);
fs/erofs/zdata.c:624, 1539; fs/f2fs/f2fs.h:2717 (in folio_get_f2fs_private helper);
fs/netfs/misc.c:237, 323; fs/orangefs/inode.c:322, 332, 470, 640, 651;
fs/ubifs/file.c:1290, 1473.

**folio_test_private() calls in B-noref context** (NFS — flag-only, no folio_get/put):
fs/nfs/file.c:368, 518, 523, 551; fs/nfs/write.c:175, 2099, 2122, 2125.

---

### Type B-noref — direct folio_set/clear_private without matching folio_get()/folio_put()

These set or clear the flag directly. The folio reference is held by a separate lifecycle mechanism.

| File | Lines | Notes |
|---|---|---|
| `fs/nfs/write.c` | 720 (folio_set_private), 747 (folio_clear_private) | Flag-only; ref held by nfs_page lifecycle (`folio_get` at request creation, `folio_put` in `nfs_clear_request()`). **Option B** — do NOT migrate to `folio_attach_private()`. Cleaned up by Coccinelle + manual pass (Tasks 10–11) after the pivot. |
| `mm/migrate.c` | 839 (folio_clear_private) | Standalone flag clear in `move_to_new_folio()`. The `folio->private = NULL` assignment for non-hugetlb folios happens at line 843 (conditional). Not paired with a matching folio_put(). Needs explicit removal (Task 11, not caught by Coccinelle pattern). |

**Resolution:**
- `fs/nfs/write.c`: Flag calls removed by Coccinelle + manual pass after pivot. No `folio_get()`
  should be added — ref is held separately.
- `mm/migrate.c:839`: Remove the standalone `folio_clear_private(folio)` call in Task 11 (the
  conditional `folio->private = NULL` at line 843 is already correct and remains).

---

### Type C — pure flag use (no folio->private data, or page->private managed separately)

These are exceptional sites that must be fixed before the semantic pivot (Tasks 2–5).

| File | Lines | Category | Fix |
|---|---|---|---|
| `mm/zsmalloc.c` | 295 (Set), 481 (Page test), 853 (Clear) | Pure bit — first-zpdesc marker. `page->private` overlaps `zpdesc->zspage` (non-NULL for all zpdescs) | Task 2: Replace with `zpdesc->zspage->first_zpdesc == zpdesc` |
| `kernel/events/ring_buffer.c` | 637 (Set), 648 (Clear) | High-order AUX page marker. Order already stored in `page->private` | Task 3: Remove Set/Clear; use `page_private() != 0` |
| `arch/x86/events/intel/bts.c` | 69 (Page test) | Reads ring_buffer AUX page high-order marker | Task 3: Replace with `page_private(page) != 0` |
| `arch/x86/events/intel/pt.c` | 777, 1292 (Page test) | Same as bts.c | Task 3: Replace with `page_private(p) != 0` |
| `drivers/xen/grant-table.c` | 878 (Set), 1034 (Page test), 1038 (Clear) | 64-bit: pure bit; 32-bit: alongside `set_page_private(ptr)`. Cleanup guard `page_private() != 0` is valid for 32-bit (non-NULL pointer). 64-bit: `page->private` is inline struct `{domid, gref}` — fields may both be 0, so NOT reliable. | Task 4: Remove Set; make 64-bit cleanup unconditional; use `page_private() != 0` guard on 32-bit only |
| `fs/crypto/crypto.c` | 205 (Set), 76 (Clear) | Set alongside `set_page_private(ciphertext_page, (unsigned long)folio)`. Pointer always non-NULL | Task 5: Remove Set/Clear; pointer suffices |

---

### F2FS zero-attach (special Type A case requiring pre-pivot fix)

| File | Line | Issue | Fix |
|---|---|---|---|
| `fs/f2fs/f2fs.h` | 2666 | `attach_page_private(page, (void *)0)` — stores NULL, violating the `folio->private != NULL` invariant that the semantic pivot will establish. Immediately followed by `set_bit(PAGE_PRIVATE_NOT_POINTER, ...)` making it non-NULL. | Task 7 Part B: Change to `attach_page_private(page, (void *)BIT(PAGE_PRIVATE_NOT_POINTER))` and drop the redundant `set_bit(PAGE_PRIVATE_NOT_POINTER, ...)` |

---

### API infrastructure (in pagemap.h)

| File | Lines | Notes | Task |
|---|---|---|---|
| `include/linux/pagemap.h` | 597 (folio_set_private in folio_attach_private), 632 (folio_test_private in folio_detach_private), 634 (folio_clear_private in folio_detach_private) | Implementation of folio_attach_private / folio_detach_private. The flag calls become no-ops after pivot; removed in Task 9. | Task 9 |
| `include/linux/pagemap.h` | 611 — `folio_change_private()` implementation | Non-NULL → non-NULL swap helper. NULL-destination callers must be fixed before the pivot (Task 7 Part A). The implementation itself gains a contract comment in Task 7. | Task 7 |

---

### PagePrivate() test-only callers (need conversion in Task 11, after pivot)

These only use `PagePrivate()` / `folio_test_private()` as a boolean test. After Task 8's
semantic pivot, `PagePrivate()` is a shim (`!!page_private(page)`). These must be converted
before Task 13 removes the shims.

| File | Lines | Conversion |
|---|---|---|
| `include/linux/buffer_head.h` | 181 — `BUG_ON(!PagePrivate(page))` in `page_buffers()` macro | `BUG_ON(!page_private(page))` |
| `fs/ceph/addr.c` | 73 — `if (PagePrivate(page))` in `page_snap_context()` | `if (page_private(page))` |
| `fs/f2fs/f2fs.h` | 2646 — `PagePrivate(page)` in `page_private_##name` macro | `page_private(page)` |
| `fs/f2fs/f2fs.h` | 2665 — `if (!PagePrivate(page))` in `set_page_private_##name` | `if (!page_private(page))` (partial fix; full fix in Task 7) |
| `drivers/md/md-bitmap.c` | 538 — `if (!PagePrivate(page))` (Type A user, guards page_buffers) | `if (!page_private(page))` |

---

### VM call sites of folio_test_private() on arbitrary folios (Task 8 Step 4)

These call `folio_test_private()` on folios that may be swapcache or hugetlb. After the pivot
(`folio_test_private()` = `!!folio->private`), swapcache (stores swp_entry_t) and hugetlb
(stores internal flags) would return true spuriously. **Explicit guards required in Task 8.**

| File | Line | Issue | Fix |
|---|---|---|---|
| `mm/migrate.c` | 1331 — `if (folio_test_private(src)) { try_to_free_buffers(src); }` | Swapcache has non-zero swp_entry_t; would trigger try_to_free_buffers incorrectly | Add `&& !folio_test_swapcache(src)` guard |
| `mm/huge_memory.c` | 4756 — `if (!folio_test_private(folio) && folio_expected_ref_count() != folio_ref_count())` | Swapcache has non-zero private → `folio_test_private()` true → skips the refcount pre-check, proceeding to unnecessary split attempt | Change to `!(folio_test_private(folio) && !folio_test_swapcache(folio))` |
| `include/trace/events/pagemap.h` | 25 — `folio_test_private(folio) ? PAGEMAP_BUFFERS : 0` | Swapcache/hugetlb would show PAGEMAP_BUFFERS flag incorrectly | Add `&& !folio_test_swapcache(folio) && !folio_test_hugetlb(folio)` |
| `include/linux/mm.h:3009` | `ref_count += folio_test_private(folio)` in `folio_expected_ref_count()` | Hugetlb (internal flags, no ref) and shmem swapcache (non-anon, reaches !anon branch) would add bogus +1 | Change to `!!folio->private && !folio_test_hugetlb(folio) && !folio_test_swapcache(folio)` |

**VM call sites confirmed safe (no guard needed):**

| File | Line | Reason |
|---|---|---|
| `mm/vmscan.c` | 960 | `folio_check_dirty_writeback()` — only called on pagecache folios in the shrink path with non-NULL mapping and active a_ops; swapcache and hugetlb are not present here |
| `mm/page-writeback.c` | 2709 | Inside `filemap_dirty_folio()` — swapcache uses `swap_aops.dirty_folio = noop_dirty_folio` and is not dirtied via this path; filesystem/pagecache callers only |

---

## Step 4: B-noref site record

> **Detailed analysis:** See `B_noref.md` for:
> - Whether NFS can be converted to Type A (yes, but requires restructuring and adds an
>   extra folio_get/put pair; Option B is preferable — see Finding 1)
> - Whether `folio_set_private()` + `folio->private` can be moved to
>   `nfs_page_assign_folio()` (no — three blockers: subreqs also call it; needs
>   `i_private_lock`; semantic ordering — see Finding 2)

| Site | File:Line | Ref mechanism | Resolution |
|---|---|---|---|
| NFS set | `fs/nfs/write.c:720` | `folio_get()` at `nfs_page_assign_folio()` (pagelist.c:411); `folio_put()` at `nfs_clear_request()` (pagelist.c:555). Ref held by nfs_page lifecycle, not by private data attachment. | **Option B**: conversion to Type A possible but requires extra folio_get/put pair and spinlock-split teardown — not worthwhile (see `B_noref.md` Finding 1). Remove standalone flag calls in Tasks 10–11. |
| NFS clear | `fs/nfs/write.c:747` | Same lifecycle | Same. See `B_noref.md` Finding 2 for why private cannot be moved to nfs_page_assign_folio(). |
| migrate clear | `mm/migrate.c:839` | Dead code — always a no-op (see `cleanup.md`) | Remove standalone `folio_clear_private(folio)` in Task 11. `folio->private = NULL` at line 843 handles the data field. |

---

## Step 5: Raw folio->private writers that do NOT set PG_private

Commands run:
```bash
# Direct folio->private writes
git grep -rn 'folio->private\s*=' -- '*.c' '*.h' \
    | grep -v 'folio_attach_private\|folio_change_private\|folio_detach_private\|private_2'

# Broader ->private= pattern (catches page->private and indirect writes)
git grep -rn -e '->private[[:space:]]*=' -- '*.c' '*.h' \
    | grep -E '\bpage->private\b|\bpages\[.*\]->private\b|\bp->private\b' \
    | grep -v 'private_2\|folio->private'

# set_page_private() callers (not accompanied by SetPagePrivate)
git grep -rn '\bset_page_private\b' -- '*.c' '*.h' | grep -v 'private_2'

# folio_has_private() callers
git grep -rn 'folio_has_private(' -- '*.c' '*.h'

# Atomic/bitops via &page_private() and &folio->private
git grep -rn '&page_private(\|&folio->private' -- '*.c' '*.h' | grep -v 'private_2'
```

### folio->private raw writes (pagecache-only concern)

| Site | What stored | pagecache? | Assessment |
|---|---|---|---|
| `fs/erofs/data.c:266,287` | In-flight I/O counter via `erofs_onlinefolio_init()` | **Yes** — live locked pagecache folios (callers: `fs/erofs/fileio.c:97`, `fs/erofs/zdata.c:1017`) | **MUST FIX before Task 8.** `/proc/kpageflags` snapshots locklessly; after pivot `folio_has_private()`, `folio_expected_ref_count()`, and KPF_PRIVATE treat it as FS private data. Also `fs/erofs/data.c:271,279,283` mutates it with atomic ops. Requires mandatory redesign in Task 7b. |
| `fs/erofs/zdata.c:1911` | Readahead folio linked-list pointer | **Yes** — `readahead_folio(rac)` returns live pagecache folios; `z_erofs_aops` has no `release_folio`, so `folio_needs_release()` falls through to `try_to_free_buffers()` | **MUST FIX before Task 8.** Fix in Task 7b: clear `folio->private` before folio is released. |
| `fs/erofs/zdata.c:569` | `Z_EROFS_PREALLOCATED_FOLIO` marker | Newly allocated, NOT yet in page cache | Safe: cleared at line 1515–1516 before cache insertion. Verify ordering is unconditional. |
| `fs/erofs/zdata.c:1516` | `NULL` (clear of preallocated marker) | See above | Part of the same safe sequence. |
| `fs/erofs/zdata.c:1577` | `Z_EROFS_SHORTLIVED_PAGE` marker | Non-pagecache decompression work pages (via `erofs_allocpage()` from pagepool or buddy) | Safe: shortlived pages are temporary working pages, not in any page cache. |
| `fs/f2fs/f2fs.h:2660,2680` | Bit-flag value `(void *)v` in macro else-branch | Yes (pagecache) | Safe: only reached when `folio->private` already non-NULL (PG_private already set). Flags-only update. |
| `fs/f2fs/f2fs.h:2720` | ORed flag bits | Yes (pagecache) | Safe: same — `folio->private` already non-NULL (in folio_set_f2fs_data() else-branch). |
| `fs/nfs/write.c:721,746` | nfs_page pointer / NULL | Yes | B-noref (analyzed in Step 4). |
| `include/linux/pagemap.h:596,615,635` | API implementation | — | `folio_attach_private`, `folio_change_private`, `folio_detach_private` internals. |
| `mm/migrate.c:649` | Copies swp_entry_t to newfolio for swapcache migration | Yes (swapcache) | Safe: swapcache excluded from `folio_test_fs_private()`. |
| `mm/migrate.c:845` | `NULL` (clear private on source after migration) | Yes | Part of `move_to_new_folio()` teardown. Safe: data already transferred to dst. |
| `mm/page_alloc.c:3091,3105` | Compound page batching order | No (allocator compound pages) | Safe: not pagecache. |

### page->private raw writes (via direct assignment or set_page_private)

All sites below store data in `page->private` without `SetPagePrivate`. Since `folio_test_fs_private()` guards on `mapping && !is_anon`, and all these sites have `page->mapping == NULL` (non-pagecache), they are **safe** after the pivot for the KPF/folio_has_private consumers.

| Site | What stored | `page->mapping` | Assessment |
|---|---|---|---|
| `arch/x86/xen/mmu_pv.c:1477` | user_pgd pointer in PGD page | NULL | Safe. |
| `arch/arm64/kvm/mmu.c:244` | KVM stage-2 level | NULL | Safe. |
| `arch/x86/kvm/mmu/mmu.c:2349` | KVM shadow page struct | NULL | Safe. |
| `arch/x86/kvm/mmu/tdp_mmu.c:235` | KVM TDP MMU shadow page | NULL | Safe. |
| `block/blk-mq.c:3630` | Request page order (linked list) | NULL (raw alloc_page) | Safe. |
| `drivers/android/binder_alloc.c:301` | Binder shrinker metadata | NULL | Safe. |
| `drivers/block/drbd/drbd_bitmap.c:200` | Bit index (with `&page_private()` bitops) | NULL (mempool pages) | Safe: drbd bitmap pages are not pagecache. |
| `drivers/block/drbd/drbd_receiver.c:80,86,147` | Page chain pointer | NULL (mempool pages) | Safe. |
| `drivers/block/null_blk/main.c:1028` | Sector index in null_blk t_page | NULL (internal alloc) | Safe. |
| `drivers/gpu/drm/ttm/ttm_pool.c:189,216` | Order or DMA struct pointer | NULL (GPU memory) | Safe. |
| `drivers/net/ethernet/sun/niu.c:3321` | RX base address | NULL (network RX page) | Safe. |
| `drivers/net/virtio_net.c:696` | Chain pointer (cleared to 0) | NULL (receive buffer page) | Safe. |
| `fs/erofs/decompressor.c:104,364,413` | `Z_EROFS_SHORTLIVED_PAGE` | NULL (temporary decomp page from `erofs_allocpage()`) | Safe: shortlived pages are non-pagecache. |
| `fs/erofs/decompressor_crypto.c:115` | `Z_EROFS_SHORTLIVED_PAGE` | NULL | Safe. |
| `fs/erofs/internal.h:469` (`erofs_pagepool_add`) | Linked-list pointer for pagepool | NULL (temporary pages) | Safe: pagepool pages are non-pagecache working pages. |
| `fs/erofs/zdata.c:194` | `Z_EROFS_SHORTLIVED_PAGE` | NULL | Safe. |
| `fs/crypto/crypto.c:75,206` | NULL (clear) or folio pointer | NULL (bounce page) | Handled in Task 5: `SetPagePrivate` being removed; pointer != NULL is the guard after. |
| `kernel/events/ring_buffer.c:638` | AUX page order | NULL (perf AUX page) | Handled in Task 3. |
| `kernel/kexec_core.c:289` | Page order | NULL (kexec reserved, `SetPageReserved`) | Safe. |
| `kernel/liveupdate/kexec_handover.c:408,503` | Kexec handover data | NULL | Safe. |
| `kernel/relay.c:124` | Relay buf pointer | NULL (raw `alloc_page`) | Safe. |
| `mm/balloon.c:34,50` | Balloon struct pointer / clear | NULL (balloon pages use `PageMovableOps`, not pagecache) | Safe. |
| `mm/debug_page_alloc.c:42,50` | Page order | NULL (allocator debug) | Safe. |
| `mm/memory-failure.c:1323,1329` | `MAGIC_HWPOISON` sentinel / clear | May have mapping; but page is hwpoisoned and unmapped from pagecache before this is called | Safe: hwpoisoned pages are isolated from LRU and reclaim before `SetPageHWPoisonTakenOff` is called. The `mapping && !is_anon` guard in KPF_PRIVATE excludes most; any residual case is an accepted edge case. |
| `mm/page_alloc.c:716,859,1829,1549` | Buddy page order | NULL (buddy allocator) | Safe. |
| `mm/percpu.c:256` | pcpu chunk pointer | NULL | Safe. |
| `net/core/skbuff.c:2026` | skb head fragment pointer | NULL (network pages) | Safe. |
| `drivers/xen/grant-table.c:876` | `xen_page_foreign` pointer (32-bit) | NULL | Handled in Task 4. |

### &page_private() and &folio->private (atomic / bitops)

| Site | What stored | Assessment |
|---|---|---|
| `drivers/block/drbd/drbd_bitmap.c:214,221,231–292` | Multiple bit flags via bitops on bitmap pages | Non-pagecache (mempool pages); safe. |
| `fs/erofs/data.c:271,279,283` | In-flight I/O counter via atomic_inc/cmpxchg on `folio->private` | **MUST FIX** — same as erofs/data.c:266. These are atomic mutations of the same counter on live pagecache folios. Requires Task 7b mandatory fix. |
| `fs/f2fs/f2fs.h:2647,2648,2667,2668,2684` | F2FS bit flags via bitops on `&page_private(page)` | Safe: always called when PG_private is already set (Type A context). After pivot, `page_private(page)` is non-NULL whenever these run. |
| `include/linux/hugetlb.h:602,609,616` | Hugetlb internal flags stored inline at `&folio->private` | Safe: hugetlb is excluded from `folio_test_fs_private()` and `folio_has_private()`. |

### folio_has_private() callers (Step 7 — verified)

All callers are in contexts where the guards in the new `folio_has_private()` (via `folio_test_fs_private()`) are correct:

| File | Line | Context | Safe after Task 8? |
|---|---|---|---|
| `mm/internal.h:686` | `folio_needs_release()` | Central trigger for `release_folio()` / `filemap_release_folio()`. Uses updated `folio_has_private()` with swapcache/hugetlb guards. | Yes |
| `mm/truncate.c:334` | Refcount accounting in `truncate_cleanup_folio()` | Adds 1 for private data. Guards prevent swapcache/hugetlb over-count. | Yes |
| `mm/truncate.c:647` | `BUG_ON(folio_has_private())` after truncation | Asserts all private data released. Correct with new guards. | Yes |
| `mm/migrate_device.c:559` | `extra += 1 + folio_has_private()` for device migration refcount | Same guards as expected_ref_count. Correct. | Yes |
| `fs/fuse/dev.c:1159` | `WARN_ON(folio_has_private(oldfolio))` | Asserts fuse does not attach private data. Correct. | Yes |

### Summary of new findings from Step 5

**Mandatory pre-Task-8 fixes (Task 7b):**
1. `fs/erofs/data.c:266,271,279,283,287` — in-flight read counter on live pagecache folios; affects `folio_has_private()`, `folio_expected_ref_count()`, and KPF_PRIVATE. Counter must be moved out of `folio->private`.
2. `fs/erofs/zdata.c:1911` — readahead linked list on live pagecache folios; `z_erofs_aops` has no `release_folio`, so `folio_needs_release()` would misfire. Must clear `folio->private` before folio unlock.

**Safe — no action required:**
All `page->private` and `set_page_private()` uses outside EROFS data.c are either:
- Non-pagecache pages (`folio->mapping == NULL`) — excluded by the `mapping && !is_anon` guard in KPF_PRIVATE and unreachable from `folio_has_private()` consumers
- Already handled by prior Tasks (Xen Task 4, crypto Task 5, ring_buffer Task 3)
- API infrastructure (pagemap.h, mm_types.h)

---

## Step 6: folio_change_private() callers

| File | Line | Data arg | Old data | Assessment |
|---|---|---|---|---|
| `mm/hugetlb.c` | 1431 | `NULL` | Internal hugetlb flag bits | **Non-Type-A**: hugetlb stores flags in folio->private without folio_attach_private(). Do NOT replace with folio_detach_private() — that would call folio_put() on a non-refcounted folio. Fix in Task 7: change to `folio->private = NULL`. |
| `fs/netfs/buffered_read.c` | 463 | `group` (non-NULL, guarded by `if (group)`) | `finfo` struct (non-NULL) | Non-NULL → non-NULL. OK. |
| `fs/netfs/buffered_write.c` | 371 | `netfs_get_group(netfs_group)` where `netfs_group != NULL` | `NETFS_FOLIO_COPY_TO_CACHE` (non-NULL constant) | Non-NULL → non-NULL. OK. |
| `fs/netfs/buffered_write.c` | 378 | `netfs_group` where `netfs_group != NULL` (WARN_ON_ONCE guards above) | `finfo` struct (non-NULL) | Non-NULL → non-NULL. OK. |
| `fs/netfs/buffered_write.c` | 597 | `netfs_get_group(netfs_group)` where `netfs_group != NULL` | `NETFS_FOLIO_COPY_TO_CACHE` (non-NULL constant) | Non-NULL → non-NULL. OK. |
| `fs/netfs/read_collect.c` | 62 | `finfo->netfs_group` where `finfo != NULL && finfo->netfs_group != NULL` | `finfo` struct (non-NULL) | Non-NULL → non-NULL. OK. |
| `include/linux/pagemap.h` | 611 | (implementation) | (implementation) | API itself, will be updated in Task 7. |

**folio_change_private() NULL-destination summary:** Only `mm/hugetlb.c:1431` passes NULL, and it
is a non-Type-A user. No Type A user passes NULL. No NULL-source issue found (all callers that
use `folio_change_private()` to attach have non-NULL private already set).

---

## Summary table

| Category | Count | Files |
|---|---|---|
| **A** (attach/detach) | ~80 sites | btrfs, buffer, ceph, erofs, f2fs, iomap, jfs, netfs, orangefs, ubifs, md-bitmap, mm/migrate |
| **B-noref** | 3 sites | fs/nfs/write.c (×2), mm/migrate.c (×1) |
| **C** (pure flag) | 13 sites | zsmalloc (3), ring_buffer (2), bts (1), pt (2), xen/grant-table (3), crypto (2) |
| **F2FS zero-attach** | 1 site | fs/f2fs/f2fs.h:2666 |
| **PagePrivate test-only** | 5 sites | buffer_head.h, ceph/addr.c, f2fs/f2fs.h (×2), md-bitmap.c |
| **VM call sites (needs guard)** | 4 sites | mm/migrate.c:1331, mm/huge_memory.c:4756, include/trace/events/pagemap.h:25, include/linux/mm.h:3009 |
| **VM call sites (safe)** | 2 sites | mm/vmscan.c:960, mm/page-writeback.c:2709 |
| **API infrastructure** | 4 entries | include/linux/pagemap.h (folio_attach_private flags ×3; folio_change_private ×1) |
| **folio_change_private (NULL-dest)** | 1 site | mm/hugetlb.c:1431 |
| **folio_change_private (OK)** | 5 sites | fs/netfs/* |

---

## folio_has_private() callers

`folio_has_private()` is defined in `include/linux/page-flags.h:1206` and currently tests
`PG_private | PG_private_2` via `PAGE_FLAGS_PRIVATE`. After the pivot (Task 8), it will test
`!!folio->private || folio_test_private_2()` with explicit swapcache/hugetlb exclusions. All
callers below trigger `release_folio()` or similar; they are safe because the updated
`folio_has_private()` will carry the necessary guards.

| File | Line | Usage |
|---|---|---|
| `mm/internal.h` | 681 — `folio_needs_release()` | Returns true if `folio_has_private() \|\| mapping_release_always()`. Drives `filemap_release_folio()` in reclaim/truncate. Safe after guard update. |
| `mm/truncate.c` | 334 — `truncate_cleanup_folio()` | Adds 1 to accounting if has_private. Safe. |
| `mm/truncate.c` | 647 — `BUG_ON(folio_has_private(folio))` | Assert that no private data remains after truncation. Safe. |
| `mm/migrate_device.c` | 559 — `extra += 1 + folio_has_private(folio)` | Refcount accounting during device migration. Safe after guard excludes swapcache/hugetlb. |
| `fs/fuse/dev.c` | 1159 — `WARN_ON(folio_has_private(oldfolio))` | Assert that fuse does not attach private data. Safe. |

**Resolution:** All handled by Task 8 Step 5 which updates `folio_has_private()` to exclude
swapcache and hugetlb. No per-caller fixes needed.

---

## Non-call-site PG_private dependencies

These files do not call PG_private accessors but reference the enum value or the derived bit number
directly. Removing `PG_private` from the enum breaks them. They must be handled in Task 8 or Task 13.

| File | Line | What it does | Task |
|---|---|---|---|
| `include/linux/page-flags.h` | 108 — `PG_private` in enum pageflags; 581 — `PAGEFLAG(Private, private, PF_ANY)` (generates all PagePrivate/folio_set_private/etc symbols); 1173 — `PAGE_FLAGS_CHECK_AT_FREE`; 1198 — `PAGE_FLAGS_PRIVATE`; 1206 — `folio_has_private()` | Core enum and derived masks. `PAGEFLAG` macro replaced with inline shims in Task 8 Step 1. `PAGE_FLAGS_CHECK_AT_FREE` must drop `PG_private` in Task 8 to avoid `bad_page()`. `PAGE_FLAGS_PRIVATE` and `folio_has_private()` updated in Task 8. Shims and enum entry removed in Task 13. | Task 8 (macro replacement, mask updates) + Task 13 (shim + enum removal) |
| `fs/proc/page.c` | 245 — `u \|= kpf_copy_bit(k, KPF_PRIVATE, PG_private)` | Exports `KPF_PRIVATE` (bit 35) in `/proc/kpageflags`. Once `folio_set_private()` is a no-op (Task 8), this bit will permanently read 0 for new attachments, making it useless. | Task 8: Remove this line and remove `KPF_PRIVATE` definitions from `include/linux/kernel-page-flags.h` and `tools/mm/page-types.c`. |
| `kernel/vmcore_info.c` | 219 — `VMCOREINFO_NUMBER(PG_private)` | Exports the `PG_private` enum value into vmcoreinfo for crash dump analysers (e.g. crash, makedumpfile). | Task 13: Remove this line. Update `Documentation/admin-guide/kdump/vmcoreinfo.rst`. |

---

## Clarifications

> **See `task_1_clarification.md` for detailed write-ups of the following:**
>
> - **Xen grant-table (Task 4):** What "make 64-bit cleanup unconditional" means — why
>   `page_private() != 0` is not a reliable guard on 64-bit (`{domid=0, gref=0}` is a
>   valid mapped state), why unconditional zeroing is safe (PageForeign is cleared by
>   gnttab_unmap_refs() before clear_private is called), and why it is actually more
>   correct than the current behaviour (eliminates stale inline struct data in recycled pages).
>
> - **f2fs (Task 7 Part B):** Whether `page->private != NULL` with `PagePrivate` not set
>   is possible in f2fs — **No**: all set paths go through attach_private (which sets both
>   together); all clear paths go through detach_private (which zeros both together); direct
>   `folio->private` assignments only occur in else-branches where PG_private is already set.
>   The only abnormal state is the reverse: `page->private == NULL` with `PagePrivate` set
>   (the zero-attach transient in `set_page_private_##name`), which Task 7 Part B already
>   fixes.

## Decisions feeding later tasks

- **Task 2**: zsmalloc — pointer comparison
- **Task 3**: ring_buffer + bts/pt — `page_private() != 0`
- **Task 4**: xen/grant-table — remove Set, unconditional 64-bit cleanup, `page_private() != 0` on 32-bit
- **Task 5**: crypto — remove Set/Clear
- **Task 7 Part A**: hugetlb `folio_change_private(folio, NULL)` → `folio->private = NULL`
- **Task 7 Part B**: f2fs zero-attach → `BIT(PAGE_PRIVATE_NOT_POINTER)`
- **Task 8**: VM call sites need swapcache/hugetlb guards (listed above)
- **Tasks 10–11 (B-noref)**: NFS — Option B (flag calls removed by Coccinelle + manual pass); mm/migrate.c:839 — standalone `folio_clear_private()` removal in Task 11 manual pass
- **Task 10** (Coccinelle): Will catch FS `folio_set_private` / `folio_clear_private` paired calls;
  will also catch NFS:720 and NFS:747 (even though order is reversed in NFS:720, both lines are covered)
- **Task 11** (manual): `mm/migrate.c:839` standalone `folio_clear_private`, PagePrivate() test callers
