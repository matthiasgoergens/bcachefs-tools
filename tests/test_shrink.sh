#!/bin/bash
#
# Tests for filesystem shrink (bcachefs device resize).
# Most tests run entirely in userspace on image files.  The data
# integrity test requires kernel bcachefs for mount/unmount.
# Root is required for loop device and mount operations.
#
# Usage: sudo tests/test_shrink.sh [path-to-bcachefs-binary]

set -euo pipefail

BCACHEFS="${1:-./target/release/bcachefs}"
if [[ ! -x "$BCACHEFS" ]]; then
	echo "Error: $BCACHEFS not found or not executable" >&2
	exit 1
fi
TEST_TMPDIR="$(mktemp --directory)"
IMG="$TEST_TMPDIR/test.img"
PASS=0
FAIL=0

cleanup() {
	mountpoint --quiet "$TEST_TMPDIR/mnt" 2>/dev/null && umount "$TEST_TMPDIR/mnt" || true
	losetup -j "$IMG" 2>/dev/null | cut --delimiter=: --fields=1 | while read -r dev; do
		losetup --detach "$dev" 2>/dev/null || true
	done
	rm --recursive --force "$TEST_TMPDIR"
}
trap cleanup EXIT

# Helper: format a fresh image via loop device, then detach
format_image() {
	local size="$1"
	rm --force "$IMG"
	truncate --size "$size" "$IMG"
	local dev
	dev="$(losetup --find --show "$IMG")"
	echo y | "$BCACHEFS" format "$dev" >/dev/null 2>&1
	losetup --detach "$dev"
}

# Helper: run a test, report pass/fail
run_test() {
	local name="$1"
	shift
	echo -n "TEST: $name ... "
	if "$@" >"$TEST_TMPDIR/output.log" 2>&1; then
		echo "PASS"
		PASS=$((PASS + 1))
	else
		echo "FAIL (exit $?)"
		cat "$TEST_TMPDIR/output.log"
		FAIL=$((FAIL + 1))
	fi
}

# Helper: fsck must pass clean (exit 0)
fsck_clean() {
	"$BCACHEFS" fsck "$IMG"
}

# ---- Tests ----

test_shrink_empty() {
	format_image 1G
	"$BCACHEFS" device resize "$IMG" 512M
	fsck_clean
}

test_shrink_then_grow() {
	format_image 1G
	"$BCACHEFS" device resize "$IMG" 512M
	"$BCACHEFS" device resize "$IMG" 768M
	fsck_clean
}

test_multiple_shrinks() {
	format_image 1G
	"$BCACHEFS" device resize "$IMG" 768M
	"$BCACHEFS" device resize "$IMG" 512M
	"$BCACHEFS" device resize "$IMG" 384M
	fsck_clean
}

test_shrink_too_small() {
	format_image 1G
	# Should fail - below BCH_MIN_NR_NBUCKETS
	if "$BCACHEFS" device resize "$IMG" 1M 2>/dev/null; then
		return 1  # should have failed
	fi
	# Filesystem should still be intact
	fsck_clean
}

test_shrink_grow_shrink() {
	format_image 1G
	"$BCACHEFS" device resize "$IMG" 512M
	"$BCACHEFS" device resize "$IMG" 896M
	"$BCACHEFS" device resize "$IMG" 640M
	fsck_clean
}

test_noop_resize() {
	format_image 512M
	# Resize to same size should be a no-op
	"$BCACHEFS" device resize "$IMG" 512M
	fsck_clean
}

test_shrink_with_data() {
	format_image 1G
	local dev mnt
	dev="$(losetup --find --show "$IMG")"
	mnt="$TEST_TMPDIR/mnt"
	mkdir --parents "$mnt"

	mount --types bcachefs "$dev" "$mnt"
	# Write ~64M of data with a known checksum
	dd if=/dev/urandom of="$mnt/testfile" bs=1M count=64 status=none
	local expected
	expected="$(sha256sum "$mnt/testfile" | cut --delimiter=' ' --fields=1)"
	umount "$mnt"
	losetup --detach "$dev"

	"$BCACHEFS" device resize "$IMG" 512M
	fsck_clean

	# Remount and verify data integrity
	dev="$(losetup --find --show "$IMG")"
	mount --types bcachefs "$dev" "$mnt"
	local actual
	actual="$(sha256sum "$mnt/testfile" | cut --delimiter=' ' --fields=1)"
	umount "$mnt"
	losetup --detach "$dev"

	if [ "$expected" != "$actual" ]; then
		echo "Checksum mismatch: expected $expected, got $actual"
		return 1
	fi
}

# ---- Run ----

echo "bcachefs shrink test suite"
echo "binary: $BCACHEFS"
echo ""

run_test "shrink empty filesystem"        test_shrink_empty
run_test "shrink then grow"               test_shrink_then_grow
run_test "multiple shrinks"               test_multiple_shrinks
run_test "shrink too small (expect fail)" test_shrink_too_small
run_test "shrink-grow-shrink cycle"       test_shrink_grow_shrink
run_test "no-op resize"                   test_noop_resize
run_test "shrink with data"               test_shrink_with_data

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
