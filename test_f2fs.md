# Test plan — f2fs PG_private removal

Two *different* things can mean "test f2fs". Pick the one you need:

- **A. The f2fs commit** — *"fs/f2fs: stop using PG_private"* (Task 7). Changed the
  `page_private_##name()` / `set_page_private_##name()` macros in `fs/f2fs/f2fs.h`
  (`PagePrivate()` → `page_private()`, and zero-attach `(void *)0` →
  `(void *)BIT(PAGE_PRIVATE_NOT_POINTER)`). **This is the f2fs-specific commit; §1–§4 below.**
- **B. The fscrypt commit, on f2fs** — the "repeat on f2fs" step in `test_fscrypt.md`. Same
  bounce-page path as ext4, just an encrypted f2fs mount. See `test_fscrypt.md`; not repeated
  here.

---

## A. Testing the f2fs commit

### What changed and how to hit it

f2fs stores up to five bit-flags in `page->private` (bit 0 `PAGE_PRIVATE_NOT_POINTER` is set
whenever any flag is set, so `page->private` is always non-NULL when in use). The changed
macros set/clear/test these. Each flag is tied to a distinct feature, so the test must exercise
all of them:

| Flag | Meaning | Workload that exercises it |
|---|---|---|
| `NOT_POINTER` | base marker (always set with any flag) | any of the below |
| `INLINE_INODE` | inode page holds inline data | many **small files** (< ~3.4K) |
| `ATOMIC_WRITE` | data page from atomic-write path | `F2FS_IOC_START/COMMIT_ATOMIC_WRITE` |
| `ONGOING_MIGRATION` | data page being migrated | **GC** / page migration / defrag |
| `REF_RESOURCE` | dirty page with referenced resources | general dirty-page writeback |

The change is a functional no-op (the invariant "`page->private != NULL` whenever a flag is
attached" is preserved; the zero-attach fix just makes that true from the instant of attach).
So this is a regression check: exercise all five paths and confirm no corruption / no VM splat.

### 1. Kernel config

```
CONFIG_F2FS_FS=y
CONFIG_F2FS_FS_XATTR=y
CONFIG_F2FS_FS_COMPRESSION=y     # compress path also uses page-private flags
CONFIG_F2FS_CHECK_FS=y           # f2fs internal consistency BUG_ONs — important
CONFIG_ZRAM=m                    # scratch dev
CONFIG_MIGRATION=y CONFIG_COMPACTION=y
CONFIG_DEBUG_VM=y
CONFIG_DEBUG_VM_PGFLAGS=y        # flags unexpected page state at free
CONFIG_KASAN=y                   # UAF in attach/detach of page-private
```

`CONFIG_F2FS_CHECK_FS=y` is the f2fs analogue of DEBUG_VM — it turns on `f2fs_bug_on()`
consistency checks that fire if page-private accounting goes wrong.

Tools: `mkfs.f2fs` (f2fs-tools), `fio`, and for atomic writes either the f2fs
`tools/f2fs_io` helper or a small ioctl program (below).

### 2. mkfs + mount (enable inline + compression)

```bash
sudo modprobe zram
echo 2G | sudo tee /sys/block/zram0/disksize
sudo mkfs.f2fs -f -O extra_attr,inode_checksum,compression /dev/zram0
sudo mkdir -p /mnt/f2fs
sudo mount -t f2fs -o inline_data,compress_algorithm=lz4 /dev/zram0 /mnt/f2fs
sudo chown "$USER" /mnt/f2fs
```

### 3. Exercise each PAGE_PRIVATE flag

**INLINE_INODE — lots of tiny files** (inline data lives in the inode page, marked
`INLINE_INODE`):
```bash
mkdir /mnt/f2fs/inline
for i in $(seq 1 5000); do echo "small inline payload $i" > /mnt/f2fs/inline/f$i; done
sync
# read back + verify
for i in $(seq 1 5000); do
    [ "$(cat /mnt/f2fs/inline/f$i)" = "small inline payload $i" ] || echo "MISMATCH f$i"
done
echo "inline check done"
```

**REF_RESOURCE + general writeback — buffered write churn with fsync**:
```bash
fio --name=f2fs --directory=/mnt/f2fs --rw=randrw --bs=4k --numjobs=4 \
    --size=200M --verify=crc32c --fsync=16 --time_based --runtime=180 --group_reporting
```

**ONGOING_MIGRATION — force GC / page migration**:
```bash
# f2fs foreground GC (migrates valid data pages → sets ONGOING_MIGRATION)
for i in $(seq 1 50); do echo 1 | sudo tee /sys/fs/f2fs/zram0/gc_urgent >/dev/null; sleep 0.1; done
echo 0 | sudo tee /sys/fs/f2fs/zram0/gc_urgent >/dev/null
# and kernel memory compaction (folio migration of movable f2fs data pages)
echo 1 | sudo tee /proc/sys/vm/compact_memory >/dev/null
```

**ATOMIC_WRITE — atomic-write ioctl path** (sets `ATOMIC_WRITE` on the data pages):
```bash
# easiest: f2fs-tools f2fs_io if available
f2fs_io write 4 0 512 zero atomic_commit /mnt/f2fs/atomicfile 2>/dev/null \
  || echo "(f2fs_io not present — use the tiny C program below)"
```
Minimal C reproducer if `f2fs_io` isn't packaged:
```c
/* gcc -o atomicwr atomicwr.c ; ./atomicwr /mnt/f2fs/af */
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/f2fs.h>          /* F2FS_IOC_START_ATOMIC_WRITE / _COMMIT_ATOMIC_WRITE */
#include <stdio.h>
int main(int c, char **v) {
    int fd = open(v[1], O_RDWR|O_CREAT, 0644);
    if (fd < 0) return perror("open"), 1;
    if (ioctl(fd, F2FS_IOC_START_ATOMIC_WRITE)) return perror("start"), 1;
    for (int i = 0; i < 256; i++) { char b[4096]; pwrite(fd, b, 4096, (long)i*4096); }
    if (ioctl(fd, F2FS_IOC_COMMIT_ATOMIC_WRITE)) return perror("commit"), 1;
    close(fd); puts("atomic write OK"); return 0;
}
```

**Compression path** (also uses page-private flags on cluster pages):
```bash
mkdir /mnt/f2fs/comp
sudo f2fs_io setflags compression /mnt/f2fs/comp 2>/dev/null || \
    chattr +c /mnt/f2fs/comp 2>/dev/null || true
for i in $(seq 1 200); do yes "compressible f2fs data $i" | head -c 1M > /mnt/f2fs/comp/c$i; done
sync
```

### 4. The real coverage — xfstests (fstests) on f2fs

These are **fstests config variables**, not compile-time macros. Put them in
`configs/<hostname>.config` (preferred) or export them before `./check`. fstests reads them to
decide what/where to test.

**Core variables — what each means:**

| Variable | Meaning / requirement |
|---|---|
| `FSTYP` | filesystem under test — `f2fs`. |
| `TEST_DEV` | **Persistent** block device, pre-formatted with `mkfs.f2fs`. Holds regression state; fstests mounts it at `TEST_DIR` and does **not** reformat it between tests. |
| `TEST_DIR` | mountpoint for `TEST_DEV` (must already exist). |
| `SCRATCH_DEV` | **Scratch** block device. `_scratch_mkfs` **reformats it for every test that needs it** — this is where most write/feature tests actually run. |
| `SCRATCH_MNT` | mountpoint for `SCRATCH_DEV` (must already exist). |
| `MKFS_OPTIONS` | extra args passed to `mkfs.f2fs` for the scratch device — put f2fs **features** here (see below) so scratch tests exercise them. |
| `MOUNT_OPTIONS` | mount options for the scratch device (e.g. `inline_data`, `compress_algorithm=lz4`). |
| `TEST_FS_MOUNT_OPTS` | mount options for `TEST_DEV` (optional). |

**Hard requirements (the usual trip-ups):**
- `TEST_DEV` and `SCRATCH_DEV` **must be two separate block devices** — never the same one.
  (e.g. two zram devices, or two partitions.)
- Both devices must be **unmounted** when you run `./check` — fstests mounts them itself.
- Both mountpoint dirs (`TEST_DIR`, `SCRATCH_MNT`) must exist beforehand.
- `TEST_DEV` must already contain a valid f2fs; `SCRATCH_DEV` need not (it gets mkfs'd), but
  must be a device fstests may destroy.

**Example `configs/localhost.config` (two zram devices):**
```bash
# create the two devices first:
#   modprobe zram num_devices=2
#   echo 2G >/sys/block/zram0/disksize; echo 2G >/sys/block/zram1/disksize
#   mkfs.f2fs -f /dev/zram0            # TEST_DEV — pre-format once
#   mkdir -p /mnt/test /mnt/scratch
export FSTYP=f2fs
export TEST_DEV=/dev/zram0
export TEST_DIR=/mnt/test
export SCRATCH_DEV=/dev/zram1
export SCRATCH_MNT=/mnt/scratch
# put page-private-touching features on the scratch device:
export MKFS_OPTIONS="-O extra_attr,inode_checksum,compression"
export MOUNT_OPTIONS="-o inline_data,compress_algorithm=lz4"
```

**Run:**
```bash
./check -g auto                      # broad; then -g quick for a fast repeat
./check -g f2fs                      # f2fs-specific group
./check -g atomicwrites              # ATOMIC_WRITE page-private path
```
Run once patched, once on baseline; result sets must be identical (the change is a no-op).

> Note: `MKFS_OPTIONS`/`MOUNT_OPTIONS` apply to the **scratch** device (the one under active
> test). Enabling `compression` + `inline_data` there is what makes the scratch tests exercise
> the `INLINE_INODE` and compression page-private flags this commit touched — otherwise a plain
> `-g auto` run may never hit them.

### 5. Pass / fail

```bash
sudo dmesg -w | grep -iE 'bad_page|VM_BUG|KASAN|f2fs.*bug|f2fs_bug_on|use-after-free'
```
- **PASS:** inline read-back matches; fio `verify=crc32c` clean; atomic write commits; GC runs;
  `-g auto` identical to baseline; dmesg clean.
- **FAIL signals for this change:**
  - `f2fs_bug_on` / `VM_BUG_ON` touching page-private accounting → a flag set/clear/test
    diverged (e.g. the zero-attach window left `page->private` momentarily NULL while marked).
  - KASAN UAF in `attach_page_private` / `detach_page_private` from f2fs → lifetime bug.
  - inline/verify mismatch → data page private-flag handling corrupted the page.

### 6. Cleanup

```bash
sudo umount /mnt/f2fs
echo 1 | sudo tee /sys/block/zram0/reset >/dev/null
sudo rmmod zram
```

---

## Notes

- `sys/fs/f2fs/<dev>/gc_urgent` node name is the backing device (`zram0` here); adjust if
  different. `cat /sys/fs/f2fs/*/` lists the tunables.
- This commit is **pre-pivot** in the series, so it runs under the old `PG_private` semantics —
  the point is that f2fs no longer *reads* the flag, using `page_private()` instead. A clean
  run confirms the macro rewrite + zero-attach fix preserved every flag's behavior.
- For the fscrypt-on-f2fs case (meaning B), follow `test_fscrypt.md` with an encrypted f2fs
  mount instead of ext4 — that tests the fscrypt commit, not this one.
