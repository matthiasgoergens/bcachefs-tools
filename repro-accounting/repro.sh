#!/usr/bin/env bash
# Two reproducers for the bcachefs per-snapshot accounting key aliasing.
#
#   A (minimal)  a filesystem the OLD tools call clean is reported by the NEW
#                tools as having accounting_mismatch, and the accounting key is
#                silently reinterpreted at the same bpos.
#
#   B (full)     the same, plus deletions, which drive the counters negative and
#                produce accounting_key_underflow persisted on disk.
#
# Runs unprivileged on plain image files: no FUSE, no loop devices, no kernel
# bcachefs module, no VM.
#
# Outside the container, point these at any two builds:
#   OLD=/path/to/old/bcachefs NEW=/path/to/new/bcachefs ./repro.sh
set -o nounset

OLD=${OLD:-/usr/local/bin/bcachefs-old}
NEW=${NEW:-/usr/local/bin/bcachefs-new}
W=${W:-/tmp/repro}

rm --recursive --force "$W"
mkdir --parents "$W"

echo "OLD tools: $("$OLD" version)"
echo "NEW tools: $("$NEW" version)"

acct() {   # print the per-snapshot accounting keys of an image
    "$NEW" list --btree=accounting "$1" 2>/dev/null > "$W/acct.txt"
    grep --extended-regexp 'snapshot id=' "$W/acct.txt" || echo "  (none)"
}

# ---------------------------------------------------------------------------
echo
echo "=========================================================================="
echo "A. MINIMAL -- the accounting key is silently reinterpreted"
echo "=========================================================================="

mkdir --parents "$W/a/seed/d"
for i in $(seq 1 20); do
    dd if=/dev/urandom of="$W/a/seed/d/f$i" bs=16k count=1 status=none
done
mkdir --parents "$W/a/seed2/d"
cp --recursive "$W/a/seed/d/." "$W/a/seed2/d/"
dd if=/dev/urandom of="$W/a/seed2/d/extra" bs=16k count=1 status=none

truncate --size=1G "$W/a/fs.img"

echo
echo "--- OLD tools create and populate it"
"$OLD" format --force --quiet --source="$W/a/seed" "$W/a/fs.img" > "$W/a/format.log" 2>&1
"$NEW" show-super "$W/a/fs.img" 2>/dev/null > "$W/a/super.txt"
grep --max-count=1 '^Version:' "$W/a/super.txt"

echo
echo "--- OLD tools consider it clean"
"$OLD" fsck --no-kernel -n "$W/a/fs.img" > "$W/a/oldfsck.log" 2>&1
echo "    old fsck exit: $?   (0 = clean)"

echo
echo "--- the accounting key as OLD tools wrote it (one counter, u64s 6)"
acct "$W/a/fs.img"

echo
echo "--- NEW tools open it read-write and add one file"
"$NEW" image update --keep-alloc --source="$W/a/seed2" "$W/a/fs.img" > "$W/a/update.log" 2>&1
echo "    image update exit: $?"

echo
echo "--- the same key afterwards (three counters, u64s 8, SAME bpos)"
acct "$W/a/fs.img"

echo
echo "--- NEW tools now report errors on it"
"$NEW" fsck --no-kernel -n "$W/a/fs.img" > "$W/a/newfsck.log" 2>&1
echo "    new fsck exit: $?"
grep --extended-regexp --after-context=6 'errors this recovery' "$W/a/newfsck.log"

# ---------------------------------------------------------------------------
echo
echo "=========================================================================="
echo "B. FULL -- deletions drive the counters negative (accounting_key_underflow)"
echo "=========================================================================="

mkdir --parents "$W/b/seed/d" "$W/b/seed2/d"
for i in $(seq 1 200); do
    dd if=/dev/urandom of="$W/b/seed/d/f$i" bs=16k count=1 status=none
done
cp --recursive "$W/b/seed/d/." "$W/b/seed2/d/"
for i in $(seq 1 150); do rm --force "$W/b/seed2/d/f$i"; done

truncate --size=2G "$W/b/fs.img"

echo
echo "--- OLD tools create and populate with 200 files"
"$OLD" format --force --quiet --source="$W/b/seed" "$W/b/fs.img" > "$W/b/format.log" 2>&1
echo "--- accounting as written by OLD tools"
acct "$W/b/fs.img"

echo
echo "--- NEW tools open it read-write and delete 150 of the 200"
"$NEW" image update --keep-alloc --source="$W/b/seed2" "$W/b/fs.img" > "$W/b/update.log" 2>&1
echo "    image update exit: $?"

echo
echo "--- accounting afterwards -- note the negative counters"
acct "$W/b/fs.img"

echo
echo "--- and fsck reports the underflow"
"$NEW" fsck --no-kernel -n "$W/b/fs.img" > "$W/b/newfsck.log" 2>&1
echo "    new fsck exit: $?"
grep --extended-regexp --after-context=1 'Accounting underflow' "$W/b/newfsck.log" | head --lines=12

echo
echo "=========================================================================="
echo "Expected:  A shows u64s 6 -> u64s 8 at an unchanged bpos, and errors where"
echo "           the old tools saw none.  B additionally shows negative counters"
echo "           persisted on disk, and 'Accounting underflow' from fsck."
echo
echo "The primary evidence is the 'list --btree=accounting' output above, not"
echo "the fsck exit code: it shows the negative values actually persisted on"
echo "disk, independent of what fsck then decides to do about them."
echo
echo "KNOWN UNRELATED ARTEFACT: 'image update --keep-alloc' leaves the image"
echo "failing fsck with 'pointer to nonexistent device 1' -> 'trigger_alloc',"
echo "which aborts fsck before it prints its error tally.  That is NOT part of"
echo "this bug -- it reproduces with the NEW tools alone, no old tools at all:"
echo "    bcachefs format --force --quiet --source=s1 fs.img"
echo "    bcachefs image update --keep-alloc --source=s2 fs.img"
echo "    bcachefs fsck --no-kernel -n fs.img       # -> trigger_alloc"
echo "Logs:      $W"
echo "=========================================================================="
