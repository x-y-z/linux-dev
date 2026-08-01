# Task 7b — EROFS validation

Runtime validation for the two Task 7b commits:
- `6f907a554c5f7` — `readahead_folio_reverse()` + `z_erofs_readahead()` conversion
- `300b2dde517da` — `erofs_onlinefolio_*()` → `folio_attach_private()`/`folio_detach_private()`

Both are read-path / decompression changes on **pagecache folios**, so EROFS's read-only
nature means most of fstests does not apply. The validation below targets the code that
actually changed: readahead reverse traversal, compound-folio handling, and the online-folio
refcount balance under reclaim/migration.

> **Run this BEFORE and AFTER Task 8.** Before the pivot, `folio_attach_private()` still sets
> `PG_private`, so this validates behavior is unchanged. After the pivot, `folio->private != NULL`
> becomes the live `folio_has_private()` signal — the reclaim-race test (Section 4) is what would
> catch any residual raw-private writer.

---

## 0. Kernel config for the test kernel

```
CONFIG_EROFS_FS=y
CONFIG_EROFS_FS_ZIP=y
CONFIG_EROFS_FS_ZIP_LZMA=y
CONFIG_EROFS_FS_ZIP_DEFLATE=y
CONFIG_EROFS_FS_ZIP_ZSTD=y
CONFIG_TRANSPARENT_HUGEPAGE=y      # large folios in page cache → exercises sibling path
# debug — failure signals surface here:
CONFIG_DEBUG_VM=y
CONFIG_DEBUG_VM_PGFLAGS=y
CONFIG_KASAN=y
CONFIG_PAGE_OWNER=y
CONFIG_DEBUG_LIST=y
```

---

## 1. Tooling

```bash
# Debian/Ubuntu
sudo apt install erofs-utils fio
# or build newest erofs-utils from source (for all compressors + tests/):
git clone https://git.kernel.org/pub/scm/linux/kernel/git/xiang/erofs-utils.git
cd erofs-utils && ./autogen.sh && ./configure && make && sudo make install
mkfs.erofs --help 2>&1 | grep -iA3 available   # confirm compressors built in
```

---

## 2. Build source tree and compressed images

```bash
SRC=/tmp/erofs-src
rm -rf "$SRC" && mkdir -p "$SRC"

# compressible content (text) — good compression ratios, multi-folio files
cp -a /usr/share/doc "$SRC"/ 2>/dev/null || true
cp -a /usr/include   "$SRC"/ 2>/dev/null || true

# a few large highly-compressible files (span many pclusters/folios)
for i in $(seq 1 8); do
    yes "erofs online folio readahead reverse test line $i" | head -c 32M > "$SRC/big_text_$i"
done

# incompressible files (exercise the non-compressed / mixed path)
for i in $(seq 1 8); do
    dd if=/dev/urandom of="$SRC/rand_$i" bs=1M count=16 status=none
done

# record source checksums for integrity comparison
( cd "$SRC" && find . -type f -exec sha256sum {} \; ) | sort > /tmp/erofs-src.sums

# --- build images; -C4096 forces small pclusters => files span many folios ---
mkfs.erofs -zlz4hc  -C4096 /tmp/erofs-lz4hc.img  "$SRC"
mkfs.erofs -zlzma   -C4096 /tmp/erofs-lzma.img   "$SRC"   # if built in
mkfs.erofs -zdeflate -C4096 /tmp/erofs-defl.img  "$SRC"   # if built in
mkfs.erofs -zzstd   -C4096 /tmp/erofs-zstd.img   "$SRC"   # if built in

# confirm multi-folio/compressed layout
dump.erofs -S /tmp/erofs-lz4hc.img
```

---

## 3. Image integrity + read correctness (per image)

```bash
IMG=/tmp/erofs-lz4hc.img
MNT=/mnt/erofs
sudo mkdir -p "$MNT"

# offline fsck first
fsck.erofs -d9 "$IMG"

sudo mount -t erofs -o loop "$IMG" "$MNT"

# cold read of the whole tree, verify checksums match source
echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
( cd "$MNT" && find . -type f -exec sha256sum {} \; ) | sort > /tmp/erofs-mnt.sums
diff /tmp/erofs-src.sums /tmp/erofs-mnt.sums \
    && echo "INTEGRITY OK ($IMG)" || echo "INTEGRITY FAIL ($IMG)"

sudo umount "$MNT"
```

Repeat for each image (`-zlzma`, `-zdeflate`, `-zzstd`).

---

## 4. Readahead + reclaim/migration race (the key test)

This is what stresses `readahead_folio_reverse()` and the online-folio attach/detach balance:
concurrent large reads (heavy readahead) while the page cache is squeezed and folios are
migrated out from under in-flight decompression.

```bash
IMG=/tmp/erofs-lz4hc.img
MNT=/mnt/erofs
sudo mount -t erofs -o loop "$IMG" "$MNT"

# run reads inside a tight memory cgroup so reclaim actively evicts EROFS folios mid-decompress
sudo mkdir -p /sys/fs/cgroup/erofs_test
echo 128M | sudo tee /sys/fs/cgroup/erofs_test/memory.max >/dev/null

# background: force folio migration repeatedly
( for _ in $(seq 1 300); do echo 1 | sudo tee /proc/sys/vm/compact_memory >/dev/null; sleep 0.2; done ) &
COMPACT=$!

# background: drop caches periodically to force cold readahead
( for _ in $(seq 1 300); do echo 1 | sudo tee /proc/sys/vm/drop_caches >/dev/null; sleep 0.5; done ) &
DROP=$!

# hammer random reads from 8 jobs, inside the cgroup.
# EROFS is READ-ONLY: fio must run --readonly against EXISTING files via --filename
# (it uses each file's real size, so no --size= and no fileset creation). Do NOT use
# --directory/--nrfiles/--size on a read-only mount — fio tries to lay out files and
# fails with "you need to specify size=".
RFILES=$(ls "$MNT"/big_text_* "$MNT"/rand_* | paste -sd: -)
sudo bash -c 'echo $$ > /sys/fs/cgroup/erofs_test/cgroup.procs; exec \
    fio --name=erofs_ra --readonly --rw=randread --bs=1M --ioengine=psync \
        --filename="'"$RFILES"'" --numjobs=8 --group_reporting \
        --time_based --runtime=120'

kill $COMPACT $DROP 2>/dev/null; wait 2>/dev/null
sudo umount "$MNT"
```

If the file list is too large for `--filename` (e.g. after `cp -a /usr/share/doc` there are
thousands of files), skip fio and use a plain shell reader — it drives kernel readahead just
as well and avoids fio's read-only-fileset quirks:

```bash
sudo mount -t erofs -o loop "$IMG" "$MNT"
reader() {
    while :; do
        find "$MNT" -type f -print0 \
            | xargs -0 -I{} dd if={} of=/dev/null bs=1M status=none 2>/dev/null
    done
}
for _ in $(seq 1 8); do reader & done
# run the compact_memory / drop_caches background loops from above alongside this
sleep 120
pkill -P $$ 2>/dev/null; wait 2>/dev/null
sudo umount "$MNT"
```

Also run a mixed order-0 / large-folio read to cover the compound-folio sibling jump in
`readahead_folio_reverse()`:

```bash
sudo mount -t erofs -o loop "$IMG" "$MNT"
echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
# large sequential reads encourage large folios in the page cache
for f in "$MNT"/big_text_*; do dd if="$f" of=/dev/null bs=2M status=none; done
for f in "$MNT"/rand_*;     do dd if="$f" of=/dev/null bs=2M status=none; done
sudo umount "$MNT"
```

---

## 5. Failure signals

Watch `dmesg` throughout Sections 3–4:

```bash
sudo dmesg -w | grep -iE 'bad_page|refcount|VM_BUG|BUG_ON|KASAN|use-after-free|list_del|corrupt'
```

Any of these indicates a Task 7b regression:
- **`bad_page` / `VM_BUG_ON_PGFLAGS`** — folio freed with `PG_private` still set, or private not cleared → online-folio detach missing/unbalanced.
- **refcount `VM_BUG_ON` / KASAN use-after-free** — `folio_attach_private()` `folio_get()` not matched by exactly one `folio_detach_private()` `folio_put()` (e.g. an `_init` with no count→0 `_end`, or a double put).
- **`INTEGRITY FAIL`** (Section 3) — readahead reverse traversal skipped/duplicated a folio.

A clean run: integrity OK for every image, no dmesg splats after the mempressure race.

---

## 6. fstests (optional, limited)

EROFS is read-only; most `generic/` tests self-skip. If running fstests anyway:

```bash
# configs/localhost.config
export FSTYP=erofs
export TEST_DEV=/tmp/erofs-test.img       # prebuilt erofs image
export TEST_DIR=/mnt/test
export SCRATCH_DEV=/tmp/erofs-scratch.img
export SCRATCH_MNT=/mnt/scratch
```

Expect many `[not run]`. The read-oriented data-integrity tests are the only relevant ones
(e.g. `generic/013`, `generic/075`, `generic/091`, `generic/263`); a full `-g auto` is not a
meaningful pass/fail for a read-only fs. The erofs-utils `tests/` suite and Sections 3–5 above
are better coverage than fstests for this change.
