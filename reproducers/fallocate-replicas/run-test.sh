#!/bin/bash
# Pure-userspace A/B for the bch2_bkey_replicas() KEY_TYPE_reservation fix.
#
#   ./run-test.sh <tools-tree> [<tools-tree> ...]
#
# For each tools tree: build it, compile test_fallocate_replicas.c against its
# headers, link it against its libbcachefs.a, format a fresh image, and run.
#
# No kernel, no VM, no loopback device. Everything runs against a plain file.

set -o errexit
set -o nounset
set -o pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="${WORK:-$here/work}"
mkdir --parents "$work"

CFLAGS_COMMON=(
    -std=gnu11 -O2 -g -Wall -fPIC
    -Wno-pointer-sign -Wno-deprecated-declarations
    -fno-strict-aliasing -fno-delete-null-pointer-checks
    -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE -D_LGPL_SOURCE
    -DRCU_MEMBARRIER -DZSTD_STATIC_LINKING_ONLY -DFUSE_USE_VERSION=35
    -DNO_BCACHEFS_CHARDEV -DNO_BCACHEFS_FS
    -DCONFIG_DEBUG_FS -DCONFIG_UNICODE -DCONFIG_STACKTRACE
    -D__SANE_USERSPACE_TYPES__ -DCONFIG_BCACHEFS_RUST=y
    -Wno-unused-but-set-variable -Wno-missing-braces -Wno-enum-conversion
    -DBCACHEFS_FUSE
)

status=0

for tree in "$@"; do
    tree="$(cd "$tree" && pwd)"
    name="$(basename "$tree")"
    echo
    echo "================ $name ($tree) ================"

    ( cd "$tree" && nice ionice make --jobs="$(nproc)" libbcachefs.a ) \
        > "$work/build-$name.log" 2>&1

    bin="$work/test-$name"
    # shellcheck disable=SC2046
    gcc "${CFLAGS_COMMON[@]}" \
        "-I$tree" "-I$tree/c_src" "-I$tree/fs" "-I$tree/include" "-I$tree/raid" \
        $(pkg-config --cflags "blkid uuid liburcu libsodium zlib liblz4 libzstd libudev libkeyutils libunwind") \
        -o "$bin" \
        "$here/test_fallocate_replicas.c" \
        "$tree/libbcachefs.a" \
        $(pkg-config --libs "blkid uuid liburcu libsodium zlib liblz4 libzstd libudev libkeyutils libunwind") \
        -lm -lpthread -lrt -lkeyutils -laio -ldl

    # 2 GiB: at 512 MiB the journal buckets come out at 256 KiB, which is
    # smaller than a single journal entry reservation, and the fs wedges with
    # "Journal stuck? ... journal_full" the moment anything goes read-write.
    img="$work/img-$name"
    rm --force "$img"
    truncate --size=2G "$img"
    "$tree/target/release/bcachefs" format --quiet "$img" \
        > "$work/format-$name.log" 2>&1
    # Not required, but it puts the image in a known-clean state and gives a
    # second, independent check that the tree under test can go read-write.
    # -K forces fsck to stay in userspace.
    "$tree/target/release/bcachefs" fsck -K -y -f "$img" \
        > "$work/fsck-$name.log" 2>&1

    echo "--- running ---"
    if "$bin" "$img"; then
        echo "RESULT[$name]: PASS"
    else
        echo "RESULT[$name]: FAIL"
        status=1
    fi
done

exit "$status"
