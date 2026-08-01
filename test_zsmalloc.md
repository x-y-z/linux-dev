# Test plan — zsmalloc PG_private → pointer comparison

Validates commit *"mm/zsmalloc: replace PG_private with pointer comparison"*
(`is_first_zpdesc()` now returns `zpdesc->zspage->first_zpdesc == zpdesc`;
`zpdesc_set_first()` and the `ClearPagePrivate()` in `reset_zpdesc()` removed).

## What changed and why the config matters

`is_first_zpdesc()` is `__maybe_unused` and is referenced **only** inside two
`VM_BUG_ON_PAGE()` assertions:
- `get_first_zpdesc()` (mm/zsmalloc.c:494)
- `obj_allocated()` (mm/zsmalloc.c:831)

> **`CONFIG_DEBUG_VM=y` is mandatory.** Without it, `VM_BUG_ON_PAGE()` expands to nothing,
> `is_first_zpdesc()` is optimized out, and the changed code never executes — the test would
> prove nothing.

The setup paths that *establish* the first-zpdesc marking — `create_page_chain()` (dropped
`zpdesc_set_first()`) and `reset_zpdesc()` (dropped `ClearPagePrivate()`) — run on allocation
and, most importantly, on **compaction and page migration**, where zpdescs are moved and
re-marked. Compaction is therefore the primary stress.

This change is behaviorally equivalent to the old flag (both mark exactly the first zpdesc),
so the test is a **regression check**: confirm no `VM_BUG_ON` fires and no memory corruption
in the compaction/migration paths.

---

## 0. Kernel config

```
CONFIG_ZSMALLOC=y
CONFIG_ZSMALLOC_STAT=y          # /sys/kernel/debug/zsmalloc/*/classes
CONFIG_ZRAM=m                   # easiest driver to exercise zsmalloc
CONFIG_ZSWAP=y                  # optional second backend
CONFIG_COMPACTION=y
CONFIG_DEBUG_VM=y               # REQUIRED — activates the is_first_zpdesc() asserts
CONFIG_DEBUG_VM_PGFLAGS=y
CONFIG_KASAN=y                  # optional — catches use-after-free in reset_zpdesc path
CONFIG_PAGE_OWNER=y             # optional
```

Optionally raise the max zspage chain length so `first_zpdesc` is non-trivial more often:
```
CONFIG_ZSMALLOC_CHAIN_SIZE=16   # default 8
```

Tools: `zram`/`zramctl` (util-linux), `fio`, `mkfs.ext4`.

---

## 1. Primary: zram load + zsmalloc compaction

```bash
sudo modprobe zram
echo lz4 | sudo tee /sys/block/zram0/comp_algorithm
echo 1G  | sudo tee /sys/block/zram0/disksize
sudo mkfs.ext4 -q /dev/zram0
sudo mkdir -p /mnt/zram && sudo mount /dev/zram0 /mnt/zram
sudo chmod 777 /mnt/zram

# alloc/free churn with data verification; randrw mixes sizes → varied size classes
fio --name=zram --directory=/mnt/zram --rw=randrw --bs=4k --numjobs=4 \
    --size=200M --verify=crc32c --time_based --runtime=180 --group_reporting

# THE key path: force zsmalloc compaction repeatedly. This migrates zpdescs and
# re-runs create_page_chain()/reset_zpdesc(), hitting the is_first_zpdesc() asserts.
for i in $(seq 1 100); do echo 1 | sudo tee /sys/block/zram0/compact >/dev/null; done

cat /sys/block/zram0/mm_stat
sudo umount /mnt/zram
```

Run a second round with a highly compressible dataset (so objects pack densely and more
multi-page zspages form), e.g. write large runs of repeating text before compacting:
```bash
sudo mount /dev/zram0 /mnt/zram
for i in $(seq 1 20); do yes "zsmalloc first-zpdesc regression test $i" \
    | head -c 32M > /mnt/zram/f$i; done
for i in $(seq 1 100); do echo 1 | sudo tee /sys/block/zram0/compact >/dev/null; done
sudo umount /mnt/zram
```

---

## 2. Concurrent zspage migration via memory compaction

zsmalloc pages are movable, so kernel memory compaction migrates them (exercising
`create_page_chain()` on the destination). Run this alongside the Section 1 fio load:

```bash
# in a second shell, while fio is running:
for _ in $(seq 1 300); do echo 1 | sudo tee /proc/sys/vm/compact_memory >/dev/null; sleep 0.2; done
```

For extra pressure, run inside a tight memory cgroup so reclaim churns the pool:
```bash
sudo mkdir -p /sys/fs/cgroup/zstest
echo 256M | sudo tee /sys/fs/cgroup/zstest/memory.max >/dev/null
# launch the fio command with: echo $$ > /sys/fs/cgroup/zstest/cgroup.procs; exec fio ...
```

---

## 3. Confirm multi-page zspages actually formed

If every zspage is single-page, `first_zpdesc` is trivially the only page and the assert is
weak. Verify some size classes use >1 page:

```bash
cat /sys/kernel/debug/zsmalloc/*/classes
# look at the 'pages_per_zspage' column — must be >1 for some rows
```

---

## 4. Optional: zswap backend

```bash
echo zsmalloc | sudo tee /sys/module/zswap/parameters/zpool
echo 1        | sudo tee /sys/module/zswap/parameters/enabled
# then drive swap (needs swap enabled) with a memory hog to push compressed pages
# through zsmalloc, then observe /sys/kernel/debug/zsmalloc/*/classes
```

---

## 5. Pass / fail

Watch the log throughout Sections 1–4:

```bash
sudo dmesg -w | grep -iE 'VM_BUG|BUG_ON|bad_page|zsmalloc|KASAN|use-after-free'
```

- **PASS:** clean dmesg — `zpdesc->zspage->first_zpdesc == zpdesc` correctly identifies the
  first zpdesc across allocation, free, compaction, and migration; fio `verify=crc32c` reports
  no data mismatches.
- **FAIL:** a `VM_BUG_ON_PAGE(!is_first_zpdesc(...))` splat (from `get_first_zpdesc()` or
  `obj_allocated()`) means the pointer comparison diverged from the true first zpdesc; a KASAN
  splat in the `reset_zpdesc()` / `create_page_chain()` region means the marking change left a
  dangling reference; any fio verify error means object corruption.

---

## 6. Cleanup

```bash
sudo swapoff /dev/zram0 2>/dev/null || true
echo 1 | sudo tee /sys/block/zram0/reset >/dev/null
sudo rmmod zram
sudo rmdir /sys/fs/cgroup/zstest 2>/dev/null || true
```

---

## Notes

- There is no dedicated zsmalloc selftest in-tree; zram + forced compaction under
  `CONFIG_DEBUG_VM=y` is the standard way to exercise these paths.
- The change is a functional no-op (flag vs. pointer both mark the first zpdesc), so a clean
  run is the expected result; the value of the test is catching an unexpected divergence or a
  refcount/lifetime mistake introduced by removing the flag set/clear.
