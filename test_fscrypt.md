# Test plan — fscrypt PG_private removal on bounce pages

Validates commit *"fs/crypto: stop setting PG_private on bounce page"*
(removed `SetPagePrivate(ciphertext_page)` from `fscrypt_encrypt_pagecache_blocks()` and
`ClearPagePrivate(bounce_page)` from `fscrypt_free_bounce_page()`; the `set_page_private()`
plaintext-folio pointer and everything else are unchanged).

## What changed and how to actually hit it

`fscrypt` allocates a **ciphertext bounce page** only on the **write path**, for **software
(CPU) encryption**:

`fscrypt_encrypt_pagecache_blocks()` → `fscrypt_alloc_bounce_page()` →
`set_page_private(bounce, folio)` → submit for I/O → `fscrypt_free_bounce_page()`.

Two consequences for the test:

1. **Must be a writable, software-encrypted filesystem.** Only ext4, f2fs, and ceph set
   `needs_bounce_pages = 1`. **EROFS does NOT use bounce pages** (read-only, no fscrypt hooks)
   — testing there exercises zero lines of this commit.
2. **Must NOT use inline hardware crypto.** With `-o inlinecrypt` (or a device that has an
   inline crypto engine), fscrypt skips bounce pages entirely. Use plain software fscrypt.

The bounce page is identified by `fscrypt_is_bounce_page()` == `page->mapping == NULL`, and the
plaintext folio is recovered via `page_private(bounce)` — **neither uses PG_private**, so this
change is a functional no-op. The test is a regression check: confirm write→read round-trips
and no page-flag / lifetime fallout in the alloc/use/free cycle.

---

## 0. Kernel config

```
CONFIG_FS_ENCRYPTION=y
CONFIG_FS_ENCRYPTION_ALGS=y      # AES-256-XTS etc. (software path)
CONFIG_EXT4_FS=y CONFIG_EXT4_FS_SECURITY=y
CONFIG_F2FS_FS=y  CONFIG_F2FS_FS_SECURITY=y
CONFIG_ZRAM=m                    # convenient scratch dev (no inline crypto engine)
CONFIG_DEBUG_VM=y                # bad_page checks at free
CONFIG_DEBUG_VM_PGFLAGS=y        # flags an unexpected page flag on a bounce page at free
CONFIG_KASAN=y                   # UAF in the bounce alloc/free path
```

Do **not** enable/force anything that routes the test device through inline (blk-crypto)
hardware encryption.

Tools: `fscrypt` (Google high-level tool; Debian pkg `fscrypt`) **or** `fscryptctl`
(low-level), plus `mkfs.ext4`, `mkfs.f2fs`. `xfstests -g encrypt` (Section 3) needs neither —
it drives the kernel keyring directly.

---

## 1. Quick manual smoke test (ext4) — drives alloc → I/O → free

Same kernel path either way; the two tools only differ in userspace key management.

### 1a. Using `fscrypt` (Debian `fscrypt` package)

```bash
# one-time global setup (writes /etc/fscrypt.conf); harmless to re-run
sudo fscrypt setup

# scratch fs with the encrypt feature, mounted WITHOUT inlinecrypt
sudo modprobe zram
echo 1G | sudo tee /sys/block/zram0/disksize
sudo mkfs.ext4 -F -O encrypt /dev/zram0
sudo mkdir -p /mnt/crypt && sudo mount /dev/zram0 /mnt/crypt

# enable fscrypt metadata on THIS filesystem (creates /mnt/crypt/.fscrypt)
sudo fscrypt setup /mnt/crypt

# encrypt an EMPTY dir with a passphrase protector
sudo mkdir /mnt/crypt/enc
printf 'testpass123\n' | sudo fscrypt encrypt /mnt/crypt/enc \
    --source=custom_passphrase --name=test --quiet

# the dir is root-owned (created via sudo); give it to your user so the plain
# write loop below doesn't hit EACCES.  (Ownership is unrelated to encryption.)
sudo chown "$USER" /mnt/crypt/enc

# WRITE with fsync → allocates/uses/frees bounce pages during writeback.
# Sized to fit a 1G device (20 x 32M = 640M).  For MORE alloc/free churn without
# needing space, use the write-then-delete round loop instead (see note below).
for i in $(seq 1 20); do
    dd if=/dev/urandom of=/mnt/crypt/enc/f$i bs=1M count=32 conv=fsync status=none
done
sync
( cd /mnt/crypt/enc && sha256sum f* ) | sort > /tmp/fscrypt-before.sums

# lock (evict key) + drop caches, then unlock → forces real decryption on read-back
sudo fscrypt lock /mnt/crypt/enc            # add --drop-caches if it reports the dir busy
echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
printf 'testpass123\n' | sudo fscrypt unlock /mnt/crypt/enc --quiet
( cd /mnt/crypt/enc && sha256sum f* ) | sort > /tmp/fscrypt-after.sums

diff /tmp/fscrypt-before.sums /tmp/fscrypt-after.sums \
    && echo "ROUND-TRIP OK" || echo "ROUND-TRIP FAIL"

sudo dmesg | grep -iE 'bad_page|VM_BUG|KASAN|fscrypt|use-after-free' || echo "dmesg clean"
sudo umount /mnt/crypt
```

Notes for `fscrypt`:
- `fscrypt setup` runs **twice**: once global (no arg), once per-fs (`fscrypt setup /mnt/crypt`).
- The target dir **must be empty** at `fscrypt encrypt` time.
- The encrypted dir is **root-owned** (created with sudo). Either `sudo chown "$USER"` it (as
  above) or run the write loop under `sudo bash -c '...'`, else the un-sudo'd `dd` gets EACCES.
- Writing also needs the dir **unlocked** — `fscrypt status /mnt/crypt/enc` should say
  `Unlocked: Yes`. A locked dir gives EACCES / garbled names too. Unlock before writing.
- `fscrypt lock`/`unlock` replaces the add-key/remount dance — cleaner, no umount needed. If
  lock fails with "directory in use", close any files under it and use `--drop-caches`.
- `No space left on device` (ENOSPC) is just the scratch dev filling — **not** a kernel bug;
  the writes that landed still exercised the path. Shrink the loop, grow the zram disksize, or
  use the churn loop below.

**Better stress — write-then-delete churn (space-safe, more alloc/free cycles):**
```bash
sudo chown "$USER" /mnt/crypt/enc
for round in $(seq 1 20); do
    for i in $(seq 1 10); do
        dd if=/dev/urandom of=/mnt/crypt/enc/f$i bs=1M count=32 conv=fsync status=none
    done
    sync
    rm -f /mnt/crypt/enc/f*      # free them; next round re-allocates bounce pages
done
sudo dmesg | grep -iE 'bad_page|VM_BUG|KASAN|use-after-free' || echo "dmesg clean"
```
Each round writes ~320M (fits a 1G dev), fsyncs (→ writeback → bounce alloc/use/free), then
deletes — 20 rounds ≈ 200 write/writeback cycles through the bounce-page mempool without ever
running out of space.

### 1b. Using `fscryptctl` (low-level alternative)

```bash
sudo mkdir /mnt/crypt/enc
KEYHEX=$(head -c 64 /dev/urandom | xxd -p -c9999)
KEYID=$(echo "$KEYHEX" | xxd -r -p | sudo fscryptctl add_key /mnt/crypt)
sudo fscryptctl set_policy "$KEYID" /mnt/crypt/enc
# ... write loop / checksums as above ...
# read-back: umount, remount, re-add the same raw key, re-checksum
```

`conv=fsync` + `drop_caches` (or `fscrypt lock`) are essential — bounce pages are allocated
during **writeback**, so cached-only writes wouldn't exercise the path.

Repeat the whole test on **f2fs** (`mkfs.f2fs -O encrypt` — or just `mkfs.f2fs` then rely on the
per-dir policy) to cover the second bounce-page user.

---

## 2. Stress the alloc/free churn

Concurrent large writes maximize simultaneous in-flight bounce pages (the mempool is shared,
so this also exercises the low-memory GFP_NOWAIT fallback in the encrypt path):

```bash
sudo mount /dev/zram0 /mnt/crypt
KEYID=$(echo "$KEYHEX" | xxd -r -p | sudo fscryptctl add_key /mnt/crypt)
# many parallel writers into the encrypted dir
for j in $(seq 1 8); do
    ( for i in $(seq 1 40); do
        dd if=/dev/urandom of=/mnt/crypt/enc/s${j}_$i bs=1M count=16 conv=fsync status=none
      done ) &
done
wait
# add memory pressure so writeback + reclaim overlap with bounce allocation
echo 1 | sudo tee /proc/sys/vm/compact_memory >/dev/null
sync; sudo umount /mnt/crypt
```

---

## 3. The real coverage: xfstests encrypt group

Standard, thorough fscrypt testing. Run on ext4 and f2fs:

```bash
# configs/localhost.config
export FSTYP=ext4                 # then repeat with f2fs
export TEST_DEV=/dev/…            # created with -O encrypt
export SCRATCH_DEV=/dev/…
export TEST_DIR=/mnt/test
export SCRATCH_MNT=/mnt/scratch

./check -g encrypt
```

`-g encrypt` covers key add/remove, policy set/get, large buffered writes (the bounce path),
directory encryption, and read-back verification.

**Run once on your patched kernel and once on baseline; the pass/fail set must be identical**
— this change is a functional no-op, so any difference is a regression.

For extra churn: `./check -g encrypt,auto` on ext4 and f2fs (adds fsx/fsstress under
encryption).

---

## 4. Pass / fail

```bash
sudo dmesg -w | grep -iE 'bad_page|VM_BUG|KASAN|fscrypt|use-after-free'
```

- **PASS:** Section 1 round-trip checksums match; `-g encrypt` result set identical to
  baseline on ext4 and f2fs; dmesg clean.
- **FAIL signals specific to this change:**
  - `bad_page` / `VM_BUG_ON_PGFLAGS` naming a private (or other) flag on a mempool page at
    free time → a bounce page freed with unexpected page-flag state (exactly the neighborhood
    of "stopped setting PG_private"; the check confirms nothing depended on it).
  - KASAN UAF in `fscrypt_free_bounce_page` / `fscrypt_encrypt_pagecache_blocks` → lifetime
    problem in the bounce alloc/free cycle.
  - `ROUND-TRIP FAIL` / `-g encrypt` regressions → decryption broke, i.e. the
    `page_private()` plaintext-folio recovery path (unchanged by the patch, so must stay green).

---

## 5. Cleanup

```bash
sudo fscryptctl remove_key "$KEYID" /mnt/crypt 2>/dev/null || true
sudo umount /mnt/crypt 2>/dev/null || true
echo 1 | sudo tee /sys/block/zram0/reset >/dev/null
sudo rmmod zram
```

---

## Notes

- **Do not test on EROFS** — it has no fscrypt bounce-page path (read-only, `needs_bounce_pages`
  unset), so it runs none of the changed code. EROFS images are for the Task 7b readahead /
  online-folio commits (`task_7b_erofs.md`), not this one.
- **Avoid `-o inlinecrypt` and inline-crypto-capable devices** — they bypass bounce pages.
- The change is a functional no-op (bounce-page identity uses `mapping == NULL`, plaintext
  recovery uses `page_private()`; neither ever read PG_private), so a clean, baseline-identical
  run is the expected result. The test's value is catching an unexpected page-flag or lifetime
  fault introduced by dropping the flag set/clear.
