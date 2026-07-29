#!/usr/bin/env bash
#
# Self-contained reproducer for accounting_key_underflow on a filesystem last
# written by pre-2026-07-16 bcachefs code.
#
# No VM, no loopback device, no bcachefs kernel module: a plain file, two
# bcachefs binaries, and one `bcachefs fusemount` that is unmounted again
# without touching a single file.  Runs in a few seconds.
#
# WHAT IT SHOWS
# -------------
# `format --background_compression=zstd --source=<dir>` leaves every written
# extent carrying a `reconcile:` entry -- one extra u64 -- and the matching
# pending work in `reconcile_work compression`.  On the next read-write mount
# the background reconcile thread does that work: each extent is rewritten and
# loses its 8-byte reconcile entry.
#
# `__trigger_extent()` accounts that as two deltas against
# BCH_DISK_ACCOUNTING_snapshot -- (-1, -bkey_bytes(old), -sectors) for the
# overwrite and (+1, +bkey_bytes(new), +sectors) for the insert -- so the net
# is (0, -8, 0) per extent, (0, -160, 0) for 20 extents.  That is *correct*
# accounting; arm B below shows it landing on a healthy key.
#
# It only underflows in arm A because the pre-2026-07-16 key
#   {id} -> [sectors]
# aliases exactly onto the post-2026-07-16 key
#   {id, btree} -> [nr_keys, key_bytes, external_sectors]
# (`btree` reads back as 0 = extents out of the old key's zero padding).  The
# stale sectors value is inherited as nr_keys and key_bytes starts at 0, so the
# very first negative delta takes it below zero -- and it is persisted.
#
# EXPECTED OUTPUT (arm A)
#   before:  u64s 6 ... snapshot id=4294967295 btree=extents 320
#   after:   u64s 8 ... snapshot id=4294967295 btree=extents 320 -160 0
#   fsck:    accounting_key_underflow  1
#
# EXPECTED OUTPUT (arm B, same image built by current tools)
#   before:  u64s 8 ... snapshot id=4294967295 btree=extents 20 1280 320
#   after:   u64s 8 ... snapshot id=4294967295 btree=extents 20 1120 320
#   fsck:    clean
#
# Note the identical -160: same operation, same delta, correct base value.
#
# BINARIES
#   OLD -- any build predating e80608bb6 / 9b7fd1479 (both 2026-07-16).
#          v1.38.8 (tagged 2026-07-03) works.
#   NEW -- current master.
#
# USAGE
#   ./repro-accounting-underflow-v2.sh [/path/to/old/bcachefs] [/path/to/new/bcachefs]
#
# Keep WORK out of /tmp: it holds two 1G sparse images.

set -o nounset

OLD=${1:-/mnt/boot-stick/usr/bin/bcachefs}
NEW=${2:-/usr/bin/bcachefs}
WORK=${WORK:-/var/tmp/acct-underflow-repro-v2}

# Files must be big enough to become real extents and numerous enough that the
# byte delta is unambiguous.  20 x 8 KiB matches the ktest image exactly.
NR_FILES=20

unmount_fuse()
{
    fusermount3 -u "$1" 2>/dev/null || fusermount -u "$1" 2>/dev/null
}

# Show the one accounting key that matters, verbatim.
#
# `list` prints raw btree keys, so it is only trustworthy on a cleanly
# shut-down image -- on a dirty one the journal still holds unapplied deltas.
# Surface the shutdown state so a stale reading cannot be mistaken for a
# finding.
show_snapshot_key()
{
    "$NEW" list --btree=accounting "$1" > "$WORK/.list.out" 2>"$WORK/.list.err"

    grep --extended-regexp 'recovering from (un)?clean shutdown' "$WORK/.list.err" |
	sed 's/^/    [/;s/$/]/'
    grep --extended-regexp 'snapshot id=|reconcile_work compression' "$WORK/.list.out" |
	sed 's/^/    /'
}

# nr keys and total bkey_bytes in the extents btree -- the ground truth the
# snapshot key is supposed to be tracking.
show_extent_totals()
{
    "$NEW" list --btree=extents "$1" 2>/dev/null |
	awk '/^u64s/ { s += $2; n++ }
	     END { printf "    extents btree: %d keys, %d bkey_bytes\n", n, s * 8 }'
}

# Mount read-write with the NEW code and immediately unmount.  No file
# operations whatsoever -- the mount itself is the trigger.
mount_rw_and_unmount()
{
    local img=$1 mnt=$2 log=$3

    mkdir --parents "$mnt"
    "$NEW" fusemount "$img" "$mnt" > "$log" 2>&1 &
    local fuse_pid=$!

    # Wait for the mount to come up rather than guessing.
    local i
    for i in $(seq 1 30); do
	mountpoint --quiet "$mnt" && break
	sleep 0.5
    done

    # Give the background reconcile thread time to drain its queue.
    sleep 5
    unmount_fuse "$mnt"
    wait $fuse_pid 2>/dev/null

    # fusemount forks, so $fuse_pid returning does not mean the filesystem has
    # gone read-only and flushed.  Wait for the daemon to actually go away --
    # otherwise the image is left dirty and `list` shows stale btree values.
    for i in $(seq 1 60); do
	pgrep --full "fusemount $img" > /dev/null || break
	sleep 0.5
    done
    sleep 1
}

rm --recursive --force "$WORK"
mkdir --parents "$WORK/seed/data"

echo "### OLD tools: $("$OLD" version)"
echo "### NEW tools: $("$NEW" version)"
echo "### work dir:  $WORK"

i=0
while [[ $i -lt $NR_FILES ]]; do
    dd if=/dev/urandom of="$(printf '%s/seed/data/f%02d' "$WORK" "$i")" \
       bs=4k count=2 status=none
    i=$((i + 1))
done

########################################################################
echo
echo "======================================================================"
echo "ARM A -- image written by OLD tools.  This is the bug."
echo "======================================================================"

IMG_A=$WORK/old-written.img
truncate --size=1G "$IMG_A"

echo
echo "--- 1. OLD tools format + populate, with background compression queued"
"$OLD" format --force --quiet \
       --background_compression=zstd \
       --source="$WORK/seed" "$IMG_A" > "$WORK/a-format.log" 2>&1 ||
    { echo "FAIL: old format failed, see $WORK/a-format.log"; exit 1; }

"$NEW" show-super "$IMG_A" 2>/dev/null |
    grep --max-count=1 '^Version:' | sed 's/^/    /'

echo
echo "--- 2. the accounting key as OLD tools wrote it (u64s 6: one counter)"
show_snapshot_key "$IMG_A"
show_extent_totals "$IMG_A"

echo
echo "--- 3. OLD tools consider it clean"
"$OLD" fsck --no-kernel -n "$IMG_A" > "$WORK/a-oldfsck.log" 2>&1
echo "    old fsck exit: $?  (0 = clean)"

echo
echo "--- 4. read-write mount by NEW code, via FUSE.  Nothing is touched."
mount_rw_and_unmount "$IMG_A" "$WORK/mnt-a" "$WORK/a-fuse.log"
grep --extended-regexp 'starting version|Doing compatible' "$WORK/a-fuse.log" |
    sed 's/^/    /'

echo
echo "--- 5. the same key afterwards (u64s 8: the new key, aliased onto it)"
show_snapshot_key "$IMG_A"
show_extent_totals "$IMG_A"

echo
echo "--- 6. NEW tools report the underflow"
"$NEW" fsck --no-kernel -n "$IMG_A" > "$WORK/a-newfsck.log" 2>&1
echo "    new fsck exit: $?  (non-zero = errors found)"
grep --after-context=2 'Accounting underflow for' "$WORK/a-newfsck.log" |
    sed 's/^/    /'
grep --after-context=4 'errors this recovery' "$WORK/a-newfsck.log" |
    sed 's/^/    /'
grep --after-context=2 'accounting mismatch for snapshot' "$WORK/a-newfsck.log" |
    sed 's/^/    /'

########################################################################
echo
echo "======================================================================"
echo "ARM B -- control: the same image, formatted by NEW tools."
echo "The mount applies the identical -160 delta and stays healthy."
echo "======================================================================"

IMG_B=$WORK/new-written.img
truncate --size=1G "$IMG_B"

echo
echo "--- 1. NEW tools format + populate"
"$NEW" format --force --quiet \
       --background_compression=zstd \
       --source="$WORK/seed" "$IMG_B" > "$WORK/b-format.log" 2>&1 ||
    { echo "FAIL: new format failed, see $WORK/b-format.log"; exit 1; }

echo
echo "--- 2. the accounting key as NEW tools wrote it (u64s 8, all 3 counters)"
show_snapshot_key "$IMG_B"
show_extent_totals "$IMG_B"

echo
echo "--- 3. read-write mount by NEW code, via FUSE.  Nothing is touched."
mount_rw_and_unmount "$IMG_B" "$WORK/mnt-b" "$WORK/b-fuse.log"

echo
echo "--- 4. the same key afterwards"
show_snapshot_key "$IMG_B"
show_extent_totals "$IMG_B"

echo
echo "--- 5. NEW tools find nothing wrong"
"$NEW" fsck --no-kernel -n "$IMG_B" > "$WORK/b-newfsck.log" 2>&1
echo "    new fsck exit: $?  (0 = clean)"
grep --extended-regexp --ignore-case 'underflow|errors this recovery' \
     "$WORK/b-newfsck.log" | sed 's/^/    /' ||
    echo "    (no underflow, no errors)"

echo
echo "### logs and images in $WORK"
