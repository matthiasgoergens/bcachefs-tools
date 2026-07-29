#!/usr/bin/env bash
# Pure-userspace reproducer for accounting_key_underflow after a read-write
# mount by current bcachefs code of a filesystem last written by pre-2026-07-16
# code.  No kernel bcachefs module, no VM -- fusemount only.
#
# Needs two bcachefs binaries:
#   OLD  -- any build predating e80608bb6 / 9b7fd1479 (2026-07-16).
#           v1.38.8 (tagged 2026-07-03) works.
#   NEW  -- current master.
#
# Usage:  ./repro-accounting-underflow.sh /path/to/old/bcachefs /path/to/new/bcachefs
set -o nounset

OLD=${1:-/mnt/boot-stick/usr/bin/bcachefs}
NEW=${2:-/usr/bin/bcachefs}
WORK=${WORK:-/var/tmp/acct-underflow-repro}

rm --recursive --force "$WORK"
mkdir --parents "$WORK/seed/sub" "$WORK/mnt"

echo "### OLD tools: $("$OLD" version)"
echo "### NEW tools: $("$NEW" version)"

dd if=/dev/urandom of="$WORK/seed/sub/blob" bs=1M count=8 status=none

IMG=$WORK/fs.img
truncate --size=2G "$IMG"

echo
echo "### 1. create + populate with OLD tools (writes old-format accounting keys)"
"$OLD" format --force --quiet --source="$WORK/seed" "$IMG" > "$WORK/1-format.log" 2>&1
"$NEW" show-super "$IMG" 2>/dev/null > "$WORK/1-super.txt"
grep --max-count=1 '^Version:' "$WORK/1-super.txt"

echo
echo "### 2. OLD tools consider it clean"
"$OLD" fsck --no-kernel -n "$IMG" > "$WORK/2-oldfsck.log" 2>&1
echo "old fsck exit: $?  (0 = clean)"

echo
echo "### 3. the accounting key, as written by OLD tools"
"$NEW" list --btree=accounting "$IMG" 2>/dev/null > "$WORK/3-acct-before.txt"
grep --extended-regexp 'snapshot id=' "$WORK/3-acct-before.txt"

echo
echo "### 4. read-write mount with NEW code, via FUSE"
#
# key_bytes is inherited as 0 (the old key had only one counter), so it starts
# at a value far below the truth.  Adding data only pushes it up; it takes a
# DELETION to drive it below zero.  So: delete the seed data that OLD tools
# wrote, which is exactly what an ordinary filesystem does all the time.
#
"$NEW" fusemount "$IMG" "$WORK/mnt" > "$WORK/4-fuse.log" 2>&1 &
sleep 3
echo hello > "$WORK/mnt/newfile" 2>>"$WORK/4-fuse.log"
# Truncating drops extent keys in place.  Their bytes were never counted (the
# inherited key_bytes started at 0), so removing them produces a net negative
# delta -- which is the underflow.
truncate --size=1M "$WORK/mnt/sub/blob" 2>>"$WORK/4-fuse.log"
sync
sleep 3
fusermount3 -u "$WORK/mnt" 2>>"$WORK/4-fuse.log" || fusermount -u "$WORK/mnt" 2>>"$WORK/4-fuse.log"
sleep 2

echo
echo "### 5. the same accounting key afterwards"
"$NEW" list --btree=accounting "$IMG" 2>/dev/null > "$WORK/5-acct-after.txt"
grep --extended-regexp 'snapshot id=' "$WORK/5-acct-after.txt"

echo
echo "### 6. NEW tools now report the underflow"
"$NEW" fsck --no-kernel -n "$IMG" > "$WORK/6-newfsck.log" 2>&1
echo "new fsck exit: $?  (non-zero = errors found)"
grep --extended-regexp --after-context=6 'errors this recovery' "$WORK/6-newfsck.log"
grep --extended-regexp --ignore-case 'underflow' "$WORK/6-newfsck.log"

echo
echo "### logs in $WORK"
