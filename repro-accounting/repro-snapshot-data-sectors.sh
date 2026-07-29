#!/usr/bin/env bash
# snapshot_data_sectors() (fs/snapshots/check_snapshots.c:546-558) has the same
# aliasing defect as bch2_snapshot_accounting_totals(): it reads counter 2
# (external_sectors) of BCH_DISK_ACCOUNTING_snapshot {id, btree=extents}.
# A pre-2026-07-16 key at that same bpos has ONE counter holding sectors, so
# counter 2 reads 0 and the sectors of the whole snapshot are invisible.
#
# Its one caller is the dangling-child-pointer repair in check_snapshot_edge()
# (fs/snapshots/check_snapshots.c:746-759):
#
#     if (!sectors && sibling &&
#         ret_fsck_err(trans, snapshot_edge_bad,
#                      "snapshot %u child pointer %u does not exist: nothing
#                       claims %u as parent and no data is accounted to it
#                       - clearing", ...))
#             return snapshot_edge_set_ptr(trans, id, side, other_id, 0);
#
# "no data is accounted to it" is the entire safety argument for dropping the
# pointer.  On a pre-rework filesystem it is always true, so the guard licenses
# clearing the last reference to a subtree that does hold data.
#
# Three arms, identical apart from the accounting key for the missing child
# 4294967290:
#
#   none   no key at all                              -> genuinely no data
#   old    u64s 6, one counter: [256]                 <- exactly what the
#          (the pre-2026-07-16 layout, aliased onto      pre-rework tools write;
#           {id, btree=extents} by the zero padding)     see part 0 below
#   new    u64s 8, three counters: [3, 120, 256]      -> 256 external sectors
#
# arms `none` and `old` are indistinguishable to the guard even though `old`
# records 256 sectors on disk.
#
# Pure userspace, unprivileged, plain image files, no FUSE, no kernel.
#
# check_allocations is excluded from the fsck run.  That is not a thumb on the
# scale: the 1.38 -> 1.39 upgrade schedules check_snapshots but NOT
# check_allocations (fs/sb/downgrade.c:128-142), so an upgraded filesystem runs
# check_snapshots against per-snapshot counters that nothing has recomputed.
# Without the exclusion, check_allocations rewrites the counters before
# check_snapshots reads them and both arms behave alike -- shown at the end.
set -o nounset

BCACHEFS=${BCACHEFS:-/usr/bin/bcachefs}
OLD=${OLD:-/mnt/boot-stick/usr/bin/bcachefs}
W=${W:-/var/tmp/repro-snapshot-data-sectors}

# bpos of BCH_DISK_ACCOUNTING_snapshot {id=4294967290, btree=extents}.
# Verified below by decoding it back with `list accounting`.
ACCT_EXTENTS=430938189327237120:0:0

rm --recursive --force "$W"
mkdir --parents "$W/seed/d"
touch "$W/seed/d/a" "$W/seed/d/b"

echo "tools: $("$BCACHEFS" version)"

# ---------------------------------------------------------------------------
echo
echo "=========================================================================="
echo "0. what the pre-rework tools actually write (skipped if OLD is missing)"
echo "=========================================================================="
if [ -x "$OLD" ]; then
    mkdir --parents "$W/olddata/d"
    dd if=/dev/urandom of="$W/olddata/d/f1" bs=64k count=4 status=none
    truncate --size=512M "$W/old.img"
    "$OLD" format --force --quiet --source="$W/olddata" "$W/old.img" > "$W/oldformat.log" 2>&1
    echo "OLD tools: $("$OLD" version)"
    "$BCACHEFS" list --btree=accounting "$W/old.img" 2>/dev/null > "$W/oldacct.txt"
    grep 'snapshot id=' "$W/oldacct.txt"
    echo "  ^ u64s 6, a single counter, decoded by the new tools as btree=extents."
    echo "    That is the layout injected as arm 'old' below."
else
    echo "  (no old binary at $OLD -- set OLD=...)"
fi

# ---------------------------------------------------------------------------
echo
echo "=========================================================================="
echo "1. build the three arms"
echo "=========================================================================="

# A live parent 4294967291 whose children[0] names 4294967290, which does not
# exist, and whose children[1] is a real, reciprocating child 4294967289.
# That is the exact shape check_snapshot_edge()'s dangling-child branch wants:
# side == EDGE_PARENT, !other_exists, sibling != 0.
#
# Everything goes in ONE kvdb session: check_snapshots is PASS_ALWAYS, so a
# second read-write open would repair the dangling pointer before the
# accounting key could be injected.
TOPO1="set snapshot_trees 0:2 snapshot_tree root_snapshot=4294967291 master_subvol=0"
TOPO2="set snapshots 0:4294967291 snapshot parent=0 tree=2 depth=0 children[0]=4294967290 children[1]=4294967289 subvol=0 state=live"
TOPO3="set snapshots 0:4294967289 snapshot parent=4294967291 tree=2 depth=1 skip[0]=4294967291 skip[1]=4294967291 skip[2]=4294967291 subvol=0 state=live"

acct_for() {
    case "$1" in
    none) echo "" ;;
    old)  echo "set accounting $ACCT_EXTENTS accounting d[0]=256" ;;
    new)  echo "set accounting $ACCT_EXTENTS accounting d[0]=3 d[1]=120 d[2]=256" ;;
    esac
}

for arm in none old new; do
    truncate --size=512M "$W/arm-$arm.img"
    "$BCACHEFS" format --force --quiet --source="$W/seed" "$W/arm-$arm.img" \
        > "$W/format-$arm.log" 2>&1

    inject=$(acct_for "$arm")
    if [ -n "$inject" ]; then
        "$BCACHEFS" kvdb --rw --command "$TOPO1" --command "$TOPO2" --command "$TOPO3" \
            --command "$inject" "$W/arm-$arm.img" > "$W/fab-$arm.log" 2>&1
    else
        "$BCACHEFS" kvdb --rw --command "$TOPO1" --command "$TOPO2" --command "$TOPO3" \
            "$W/arm-$arm.img" > "$W/fab-$arm.log" 2>&1
    fi

    echo
    echo "--- arm $arm:"
    "$BCACHEFS" kvdb --command "list snapshots" --command "list accounting" \
        "$W/arm-$arm.img" 2>/dev/null > "$W/state-$arm.txt"
    grep --extended-regexp 'type snapshot 0:4294967291' "$W/state-$arm.txt"
    grep 'snapshot id=4294967290' "$W/state-$arm.txt" || echo "    (no accounting key for 4294967290)"
done

# ---------------------------------------------------------------------------
echo
echo "=========================================================================="
echo "2. run check_snapshots (fsck without check_allocations)"
echo "=========================================================================="

for arm in none old new; do
    cp --reflink=auto --force "$W/arm-$arm.img" "$W/run-$arm.img"
    "$BCACHEFS" fsck --no-kernel -y -o recovery_passes_exclude=check_allocations \
        "$W/run-$arm.img" > "$W/fsck-$arm.log" 2>&1

    echo
    echo "--- arm $arm: check_snapshots said:"
    grep --extended-regexp 'nothing claims|topology damage is beyond' "$W/fsck-$arm.log" \
        || echo "    (nothing)"
    echo "--- arm $arm: parent node afterwards:"
    "$BCACHEFS" kvdb --command "list snapshots" "$W/run-$arm.img" 2>/dev/null > "$W/after-$arm.txt"
    grep 'type snapshot 0:4294967291' "$W/after-$arm.txt"
done

# ---------------------------------------------------------------------------
echo
echo "=========================================================================="
echo "3. control: the same runs WITH check_allocations"
echo "=========================================================================="
for arm in old new; do
    cp --reflink=auto --force "$W/arm-$arm.img" "$W/full-$arm.img"
    "$BCACHEFS" fsck --no-kernel -y "$W/full-$arm.img" > "$W/fullfsck-$arm.log" 2>&1
    echo
    echo "--- arm $arm:"
    grep --extended-regexp 'accounting mismatch for snapshot|nothing claims|topology damage is beyond' \
        "$W/fullfsck-$arm.log"
    "$BCACHEFS" kvdb --command "list snapshots" "$W/full-$arm.img" 2>/dev/null > "$W/fafter-$arm.txt"
    grep 'type snapshot 0:4294967291' "$W/fafter-$arm.txt"
done

echo
echo "=========================================================================="
echo "Expected from step 2:"
echo "  none  'nothing claims 4294967291 as parent and no data is accounted to"
echo "         it - clearing'; children become 4294967289 0."
echo "  old   IDENTICAL to none, although 256 sectors are recorded on disk."
echo "  new   no clearing; 'snapshot topology damage is beyond"
echo "         single-corruption repair ... not repairing'; children stay"
echo "         4294967290 4294967289."
echo
echo "The only difference between arms old and new is the counter layout of one"
echo "accounting key holding the same 256 sectors."
echo
echo "Step 3 shows the defect is masked once check_allocations has recomputed"
echo "the counters -- which the 1.38 -> 1.39 upgrade does not schedule."
echo
echo "With an instrumented build (a bch_err() printing v[] in"
echo "snapshot_data_sectors) step 2 prints:"
echo "  none  INSTR snapshot_data_sectors id=4294967290 raw v=[0,0,0]     -> sectors=0"
echo "  old   INSTR snapshot_data_sectors id=4294967290 raw v=[256,0,0]   -> sectors=0"
echo "  new   INSTR snapshot_data_sectors id=4294967290 raw v=[3,120,256] -> sectors=256"
echo
echo "Logs: $W"
echo "=========================================================================="
