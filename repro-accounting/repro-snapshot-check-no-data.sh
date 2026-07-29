#!/usr/bin/env bash
# bch2_snapshot_node_check_no_data() is disarmed on filesystems written before
# the per-snapshot accounting rework (upstream e80608bb6 / 9b7fd1479, 2026-07-16).
#
# The check refuses a destructive snapshot-node transition when the per-snapshot
# accounting counters are nonzero.  On a filesystem written by older code those
# counters read as zero (or as garbage in the wrong slot), so the check licenses
# the transition and stranded keys are silently orphaned.
#
# Two parts, both pure userspace, unprivileged, on plain image files:
#
#   1. NATURAL   an image formatted by the OLD tools; fsck prints the counters
#                the current code reads vs. the counters that are true.
#
#   2. A/B       two images identical except for the presence of the per-snapshot
#                accounting key, each with a dying snapshot node holding one
#                stranded xattr.  Arm A refuses (safety net works).  Arm B - the
#                state an old filesystem is actually in - deletes the node and
#                strands the key.
#
# FUSE is used only to get a read-write open that runs delete_dead_snapshots with
# default options; `bcachefs kvdb --rw` opens with noauto_snapshot_deletion, which
# short-circuits the pass at fs/snapshots/delete.c:1204.
set -o nounset

OLD=${OLD:-/mnt/boot-stick/usr/bin/bcachefs}
NEW=${NEW:-/usr/bin/bcachefs}
W=${W:-/var/tmp/repro-check-no-data}

rm --recursive --force "$W"
mkdir --parents "$W"

echo "OLD tools: $("$OLD" version)"
echo "NEW tools: $("$NEW" version)"

# ---------------------------------------------------------------------------
echo
echo "=========================================================================="
echo "1. NATURAL -- what the current code reads on an old filesystem"
echo "=========================================================================="

# (a) a filesystem with file data
mkdir --parents "$W/seed-data/d"
dd if=/dev/urandom of="$W/seed-data/d/f1" bs=64k count=1 status=none
dd if=/dev/urandom of="$W/seed-data/d/f2" bs=64k count=1 status=none

# (b) a metadata-only filesystem: no extents at all
mkdir --parents "$W/seed-meta/d/sub"
touch "$W/seed-meta/d/a" "$W/seed-meta/d/b" "$W/seed-meta/d/c"

for kind in data meta; do
    truncate --size=512M "$W/fs-$kind.img"
    "$OLD" format --force --quiet --source="$W/seed-$kind" "$W/fs-$kind.img" \
        > "$W/format-$kind.log" 2>&1

    echo
    echo "--- $kind: per-snapshot accounting key as the OLD tools wrote it,"
    echo "    decoded by the NEW tools:"
    "$NEW" list --btree=accounting "$W/fs-$kind.img" 2>/dev/null > "$W/acct-$kind.txt"
    grep --extended-regexp 'snapshot id=' "$W/acct-$kind.txt" || echo "    (no per-snapshot accounting key at all)"

    echo "--- $kind: what check_allocations says the counters should be:"
    "$NEW" fsck --no-kernel -n "$W/fs-$kind.img" > "$W/fsck-$kind.log" 2>&1
    grep --extended-regexp --after-context=3 'accounting mismatch for snapshot' "$W/fsck-$kind.log"
done

echo
echo "  Counters are [nr_keys, key_bytes, external_sectors]."
echo "  check_no_data reads external_sectors (counter 2) always, and nr_keys"
echo "  (counter 0) once version_upgrade_complete >= 1.39."
echo "  Note the version upgrade does NOT schedule check_allocations:"
grep --max-count=1 'Upgrade requires recovery passes' "$W/fsck-data.log"

# ---------------------------------------------------------------------------
echo
echo "=========================================================================="
echo "2. A/B -- the same node, with and without the accounting key"
echo "=========================================================================="

cp "$W/fs-meta.img" "$W/base.img"

# First rw open completes the 1.38 -> 1.39 upgrade, so both arms run with
# version_upgrade_complete = 1.39, i.e. trust_keys = true.  The check fails
# open even in its strongest configuration.
"$NEW" kvdb --rw --command "list snapshots" "$W/base.img" > "$W/upgrade.log" 2>&1

# A dying leaf snapshot in its own tree, holding one xattr whose inode does not
# exist in that snapshot.  delete_dead_snapshot_keys_v2() is driven by the inodes
# btree (fs/snapshots/delete.c:807-842), so it never visits that xattr: it is
# stranded, and check_no_data is the only thing left to notice.
"$NEW" kvdb --rw \
  --command "set snapshot_trees 0:2 snapshot_tree root_snapshot=4294967290 master_subvol=0" \
  --command "set snapshots 0:4294967290 snapshot parent=0 tree=2 depth=0 subvol=0 state=will_delete" \
  --command "set xattrs 9999:12345:4294967290 xattr" \
  "$W/base.img" > "$W/fabricate.log" 2>&1

"$NEW" kvdb --command "list accounting" "$W/base.img" 2>/dev/null > "$W/base-acct.txt"
echo
echo "--- accounting for the fabricated node (correct, new-format):"
grep --extended-regexp 'snapshot id=4294967290' "$W/base-acct.txt"

ACCT_POS=$(grep --extended-regexp 'snapshot id=4294967290 btree=xattrs' "$W/base-acct.txt" \
           | sed --expression='s/.*type accounting \([0-9]*:[0-9]*:[0-9]*\).*/\1/')

cp "$W/base.img" "$W/armA.img"
cp "$W/base.img" "$W/armB.img"

# Arm B: remove the key.  Old code's only counter was sectors, and this snapshot
# has none, so old code would have written no key here at all - exactly what part
# 1 measured on fs-meta.img.
"$NEW" kvdb --rw --command "set accounting $ACCT_POS deleted" "$W/armB.img" > "$W/armB-strip.log" 2>&1

for arm in A B; do
    "$NEW" recovery-pass --set check_subvols --set delete_dead_snapshots "$W/arm$arm.img" \
        > "$W/rp$arm.log" 2>&1
    mkdir --parents "$W/mnt$arm"
    timeout 120 "$NEW" fusemount -f "$W/arm$arm.img" "$W/mnt$arm" > "$W/fuse$arm.log" 2>&1 &
    sleep 8
    fusermount3 -u "$W/mnt$arm" 2>/dev/null || fusermount -u "$W/mnt$arm" 2>/dev/null
    sleep 3

    echo
    echo "--- arm $arm: delete_dead_snapshots said:"
    grep --extended-regexp --after-context=1 'refusing to delete|delete_dead_snapshots\.\.\.' "$W/fuse$arm.log" \
        | head --lines=6
    echo "--- arm $arm: resulting node and key:"
    "$NEW" kvdb --command "list snapshots" --command "list xattrs" "$W/arm$arm.img" \
        2>/dev/null > "$W/after$arm.txt"
    grep --extended-regexp 'type snapshot 0:4294967290|type xattr 9999' "$W/after$arm.txt"
done

echo
echo "=========================================================================="
echo "Expected:"
echo "  arm A  'snapshot node 4294967290 still has 1 keys / 0 sectors accounted"
echo "          to it - refusing to delete/empty, to prevent data loss'"
echo "          and the node stays 'will_delete'."
echo "  arm B  no message; the node becomes 'deleted', its snapshot_tree is gone,"
echo "          and the xattr is still on disk, orphaned in a deleted snapshot."
echo
echo "The only difference between the images is the accounting key."
echo "Logs: $W"
echo "=========================================================================="
