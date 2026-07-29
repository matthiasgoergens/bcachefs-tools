#!/usr/bin/env bash
# During the 1.38 -> 1.39 upgrade recovery itself, `trust_keys` is FALSE.
#
# bch2_snapshot_accounting_totals() (fs/snapshots/delete.c:190-242) gates the
# nr_keys / key_bytes counters on
#
#     trust_keys = c->sb.version_upgrade_complete >= 1.39
#
# and c->sb.version_upgrade_complete is only bumped at fs/init/recovery.c:1038
# -- *after* bch2_run_recovery_passes_startup() at :961 has already run every
# pass, including the delete_dead_snapshots that the 1.39 upgrade itself
# scheduled.  So the one recovery that is supposed to clean up a
# just-upgraded filesystem runs bch2_snapshot_node_check_no_data() in its
# weakest configuration: sectors-only, and sectors is counter 2, which on a
# pre-2026-07-16 key is always 0.
#
# Measured here as an A/B on nothing but BCH_SB_VERSION_UPGRADE_COMPLETE.
# Both images are byte-identical apart from that superblock field, and both
# run the exact same seven recovery passes:
#
#   arm A  version_upgrade_complete = 1.39 (upgrade already done).
#          The seven passes are scheduled explicitly with `recovery-pass --set`.
#          -> trust_keys true -> check_no_data sees the 7 accounted keys and
#             refuses; the node stays will_delete.
#
#   arm B  version_upgrade_complete = 1.38 (upgrade pending).
#          check_version_upgrade() schedules the same seven passes itself.
#          -> trust_keys false -> nr_keys masked to 0, sectors reads 0,
#             check_no_data passes, the node is deleted while the accounting
#             still says 7 keys are accounted to it.
#
# Pure userspace on a plain image file, unprivileged: FUSE is used only
# because it is the cheapest read-write open that runs delete_dead_snapshots
# with default options -- `bcachefs kvdb --rw` sets auto_snapshot_deletion=0
# (src/commands/kvdb.rs:1293), which short-circuits the pass at
# fs/snapshots/delete.c:1202.
#
# The accounting key is injected directly rather than being produced by a real
# stranded key.  That is deliberate: the 1.38 -> 1.39 upgrade schedules
# check_inodes and check_xattrs (fs/sb/downgrade.c:128-142) but NOT
# check_allocations, so a real stranded xattr would be reaped by check_xattrs
# in arm B before delete_dead_snapshots ever ran, and nothing at all recomputes
# the per-snapshot accounting.  An injected key in a btree the upgrade does not
# check (dirents) is what an upgraded old filesystem actually looks like:
# counters nobody has recomputed.
set -o nounset

BCACHEFS=${BCACHEFS:-/usr/bin/bcachefs}
W=${W:-/var/tmp/repro-trust-keys}

# bpos of BCH_DISK_ACCOUNTING_snapshot {id=4294967290, btree=dirents}.
# Verified below by decoding it back with `list accounting`.
ACCT_DIRENTS=430938189327368192:0:0

# BCH_VERSION(1, 38) == (1 << 10) | 38
V138=1062

rm --recursive --force "$W"
mkdir --parents "$W/seed/d" "$W/mntA" "$W/mntB"
touch "$W/seed/d/a" "$W/seed/d/b"

echo "tools: $("$BCACHEFS" version)"

# ---------------------------------------------------------------------------
echo
echo "=========================================================================="
echo "base image: a dying snapshot node with 7 keys accounted to it"
echo "=========================================================================="

truncate --size=512M "$W/base.img"
"$BCACHEFS" format --force --quiet --source="$W/seed" "$W/base.img" > "$W/format.log" 2>&1

"$BCACHEFS" kvdb --rw \
  --command "set snapshot_trees 0:2 snapshot_tree root_snapshot=4294967290 master_subvol=0" \
  --command "set snapshots 0:4294967290 snapshot parent=0 tree=2 depth=0 subvol=0 state=will_delete" \
  --command "set accounting $ACCT_DIRENTS accounting d[0]=7 d[1]=99 d[2]=0" \
  "$W/base.img" > "$W/fabricate.log" 2>&1

"$BCACHEFS" kvdb --command "list accounting" --command "list snapshots" \
    "$W/base.img" 2>/dev/null > "$W/base-state.txt"
grep --extended-regexp 'snapshot id=4294967290|type snapshot 0:4294967290' "$W/base-state.txt"

# ---------------------------------------------------------------------------
cp --reflink=auto --force "$W/base.img" "$W/armA.img"
cp --reflink=auto --force "$W/base.img" "$W/armB.img"

# arm A: the upgrade is already complete; schedule by hand exactly the passes
# that the 1.39 upgrade schedules (fs/sb/downgrade.c:128-142).
"$BCACHEFS" recovery-pass \
    --set check_lrus --set check_alloc_to_lru_refs \
    --set check_inodes --set check_xattrs \
    --set check_snapshots --set check_subvols \
    --set delete_dead_snapshots \
    "$W/armA.img" > "$W/armA-rp.log" 2>&1

# arm B: pretend the upgrade has not happened yet.  The on-disk version stays
# 1.39; only VERSION_UPGRADE_COMPLETE goes back, which is exactly the state a
# 1.38 filesystem is in when the 1.39 tools first open it.
"$BCACHEFS" kvdb --nostart --command "sb set version_upgrade_complete=$V138" \
    "$W/armB.img" > "$W/armB-sb.log" 2>&1

for arm in A B; do
    echo
    echo "=========================================================================="
    echo "arm $arm: $("$BCACHEFS" kvdb --nostart --command 'sb get version_upgrade_complete' "$W/arm$arm.img" 2>/dev/null)"
    echo "=========================================================================="

    timeout 120 "$BCACHEFS" fusemount -f "$W/arm$arm.img" "$W/mnt$arm" \
        > "$W/fuse$arm.log" 2>&1 &
    sleep 8
    fusermount3 -u "$W/mnt$arm" 2>/dev/null || fusermount -u "$W/mnt$arm" 2>/dev/null
    sleep 3

    echo "--- passes run:"
    grep --extended-regexp 'Doing compatible version upgrade|Upgrade requires recovery passes|requires following recovery passes' \
        "$W/fuse$arm.log"
    echo "--- check_no_data said:"
    grep --extended-regexp 'refusing to delete' "$W/fuse$arm.log" \
        || echo "    (nothing -- the node was deleted)"
    echo "--- resulting node, tree and accounting:"
    "$BCACHEFS" kvdb --command "list snapshots" --command "list snapshot_trees" \
        --command "list accounting" "$W/arm$arm.img" 2>/dev/null > "$W/after$arm.txt"
    grep --extended-regexp 'type snapshot 0:4294967290|type snapshot_tree 0:2|snapshot id=4294967290' \
        "$W/after$arm.txt"
done

echo
echo "=========================================================================="
echo "Expected:"
echo "  arm A  'snapshot node 4294967290 still has 7 keys / 0 sectors accounted"
echo "          to it - refusing to delete/empty, to prevent data loss'; the"
echo "          node stays will_delete and its snapshot_tree survives."
echo "  arm B  no refusal at all; the node becomes 'deleted' and its"
echo "          snapshot_tree is gone.  (Whether the accounting key for"
echo "          4294967290 survives the deletion varies between runs; the"
echo "          measurement is the absence of the refusal and the state"
echo "          transition, not the key's fate afterwards.)"
echo
echo "With an instrumented build (a bch_err() in bch2_snapshot_accounting_totals"
echo "and around fs/init/recovery.c:961/1038) arm B prints, in this order:"
echo "  INSTR before run_recovery_passes_startup: sb.version=1.39 version_upgrade_complete=1.38"
echo "  delete_dead_snapshots...INSTR accounting_totals id=4294967290 ... trust_keys=0"
echo "  INSTR accounting_totals id=4294967290 btree=2 raw v=[7,99,0] -> nr_keys=0 key_bytes=0 sectors=0"
echo "  INSTR after run_recovery_passes_startup: ... version_upgrade_complete=1.38"
echo "  INSTR marking VERSION_UPGRADE_COMPLETE 1.38 -> 1.39 (after all passes)"
echo
echo "Logs: $W"
echo "=========================================================================="
