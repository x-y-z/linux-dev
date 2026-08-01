# B-noref site deep dive: fs/nfs/write.c

## Active B-noref sites

There are two active B-noref sites in the tree, both in NFS:

| File | Lines | Description |
|---|---|---|
| `fs/nfs/write.c` | 720 — `folio_set_private(folio)` | Sets flag when head request is registered with inode |
| `fs/nfs/write.c` | 747 — `folio_clear_private(folio)` | Clears flag when head request is removed from inode |

`mm/migrate.c:839` (`folio_clear_private(folio)`) was also categorised as B-noref but is
confirmed dead code in the current tree; see `cleanup.md`.

---

## Finding 1: Can NFS be converted to Type A?

**Short answer: yes, but it requires restructuring and adds an extra folio_get/put pair.**

### Current NFS folio-private lifecycle

```
nfs_page_create_from_folio()
  → nfs_page_assign_folio()       folio_get(folio)         [ref A: nfs_page owns folio]

nfs_inode_add_request()           ← called only for HEAD requests
  (under mapping->i_private_lock)
  → folio_set_private(folio)      [no ref taken]
  → folio->private = req

nfs_inode_remove_request()
  (under mapping->i_private_lock)
  → folio->private = NULL
  → folio_clear_private(folio)    [no ref dropped]
  (lock released)

nfs_clear_request()
  → folio_put(folio)              [drops ref A]
```

The `folio_get()` in `nfs_page_assign_folio()` is the ownership ref for the nfs_page
object, not specifically for the private data attachment. This is the ref that
`folio_expected_ref_count()` counts via `folio_test_private()`.

### Type A conversion

The private-data set/clear could be converted as follows:

```c
/* nfs_inode_add_request() — attach */
spin_lock(&mapping->i_private_lock);
folio_attach_private(folio, req);   /* folio_get() + folio->private = req + flag */
spin_unlock(&mapping->i_private_lock);
```

`folio_get()` inside a spinlock is safe (it is an atomic increment).

```c
/* nfs_inode_remove_request() — detach */
/* folio_detach_private() cannot be called under the spinlock:
 * it calls folio_put(), which can invoke __folio_put() → page release path.
 * Even though in practice the nfs_page-ownership ref (ref A) still exists
 * and prevents the folio from being freed, calling folio_put() under a
 * spinlock is architecturally unsafe. The teardown must be split: */
spin_lock(&mapping->i_private_lock);
folio->private = NULL;
folio_clear_private(folio);
spin_unlock(&mapping->i_private_lock);
folio_put(folio);   /* matches the folio_get() inside folio_attach_private() */
```

The resulting reference accounting (with Type A conversion):

```
nfs_page_assign_folio():    folio_get()               [ref A: nfs_page ownership]
nfs_inode_add_request():    folio_attach_private()    [ref B: private data attachment]
nfs_inode_remove_request(): folio_put()               [drops ref B]
nfs_clear_request():        folio_put()               [drops ref A]
```

### Why Option B (keep as-is) is preferable

1. **No functional benefit.** The existing ref A from `nfs_page_assign_folio()` already
   serves the same purpose as the ref that `folio_attach_private()` would take. The folio
   cannot be freed while either ref is held; adding ref B is redundant work.

2. **Spinlock split required.** `folio_detach_private()` cannot be called under
   `mapping->i_private_lock` because it calls `folio_put()`. The caller would need to
   manually split the teardown across the lock boundary (clear private under lock, put
   outside), increasing code complexity with no benefit.

3. **`folio_expected_ref_count()` is already correct for NFS.** After the pointer-based
   pivot (Task 8), `folio_expected_ref_count()` adds `!!folio->private && !hugetlb &&
   !swapcache`. For NFS folios with `folio->private = req`, this adds +1. That ref IS
   present — it is ref A from `nfs_page_assign_folio()`. The bookkeeping is correct
   whether or not the ref was taken specifically via `folio_attach_private()`.

**Resolution:** Keep NFS as B-noref (Option B). Remove only the standalone
`folio_set_private()` / `folio_clear_private()` flag calls after the semantic pivot (Tasks
10–11); the `folio->private` assignments and `folio_get()/put()` are correct and unchanged.

---

## Finding 2: Can folio_set_private() and folio->private be moved to nfs_page_assign_folio()?

**No. Three blockers prevent this.**

### Blocker 1: nfs_page_assign_folio() is called for subrequests too

`folio->private` must always point to the **head** request. This invariant is:
- Established by `nfs_inode_add_request()` (which has `WARN_ON_ONCE(req->wb_this_page != req)`,
  asserting it is only called for head requests)
- Read by `nfs_folio_find_head_request()` (`fs/nfs/write.c:170`)
- Asserted by `nfs_lock_and_join_requests()` (`fs/nfs/write.c:541`:
  `if (head != folio->private) → retry`)

But `nfs_page_assign_folio()` is called for **both** head and subrequests:

```c
/* nfs_page_create_from_folio() — head request: */
nfs_page_assign_folio(ret, folio);
nfs_page_group_init(ret, NULL);     /* NULL → this is the head */

/* nfs_create_subreq() — subrequest: */
nfs_page_assign_folio(ret, folio);  /* same call */
nfs_page_group_init(ret, last);     /* non-NULL → this is a subrequest */
```

`nfs_page_assign_folio()` is called **before** `nfs_page_group_init()`, so there is no way
to know at call time whether the request is a head or a subrequest. If `folio->private = req`
were set inside `nfs_page_assign_folio()`, each subrequest would overwrite the head's entry.

### Blocker 2: The assignment requires mapping->i_private_lock

`nfs_folio_find_head_request()` reads `folio->private` under `mapping->i_private_lock`
(line 177). The write at `nfs_inode_add_request():721` is also under this lock. At the time
`nfs_page_assign_folio()` is called, the caller has no access to the mapping's
`i_private_lock`. Moving the write there would create an unprotected window that races with
concurrent readers.

### Blocker 3: Semantic ordering — private marks the request as the active inode owner

`nfs_try_to_update_request()` reads `folio->private` to find an existing active head request
to extend rather than creating a new one. If `folio->private = req` were set at allocation
time (in `nfs_page_assign_folio()`), there would be a window between request creation and
`nfs_inode_add_request()` where `folio->private` points to a request that is not yet locked,
not yet on any list, and not yet the active inode request. `nfs_try_to_update_request()`
could find it and attempt to extend an uncommitted, partially-initialised request.

### Conclusion

The split is intentional:

- `nfs_page_assign_folio()` manages the folio **reference** (the nfs_page object's ownership
  of the folio). Called for both head and subrequests.
- `nfs_inode_add_request()` manages the folio **private pointer** (registering the head
  request as the folio's active write owner, under `i_private_lock`, at the correct point in
  the lifecycle). Called only for head requests.

These responsibilities cannot be merged.
