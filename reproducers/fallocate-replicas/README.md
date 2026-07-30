# `bch2_bkey_replicas()` and `KEY_TYPE_reservation`

A pure-userspace reproducer showing that a fallocated extent reports zero
replicas, so `bch2_check_range_allocated()` cannot tell a reservation from a
hole.

No kernel, no VM, no loopback device. Everything runs against a plain file, and
a run takes a few seconds.

## What it does

`test_fallocate_replicas.c` links `libbcachefs.a` directly and:

1. formats nothing — it opens an image the runner has already formatted;
2. creates a file and fallocates 1 MiB through `bch2_extent_fallocate()`;
3. walks the extents btree over that range and, for each key fallocate actually
   left behind, applies the same test `bch2_check_range_allocated()` applies —
   `nr_replicas <= bch2_bkey_replicas(c, k)`;
4. exits non-zero if any key fails it.

Step 3 transcribes the predicate rather than calling
`bch2_check_range_allocated()` itself, because that function lives in
`fs/vfs/direct.c`, which is compiled out under `-DNO_BCACHEFS_FS` and so is not
in `libbcachefs.a`. The keys are genuine; only the predicate is copied, and it
is marked as such in the source.

## Running it

`run-test.sh` takes one or more bcachefs-tools trees, builds each, compiles the
test against that tree's headers, links it against that tree's
`libbcachefs.a`, formats a fresh image with that tree's `bcachefs` binary, and
runs. That makes an A/B against two trees a single command:

```sh
./reproducers/fallocate-replicas/run-test.sh /path/to/tree-without-fix /path/to/tree-with-fix
```

Expected output:

```
RESULT[tree-without-fix]: FAIL
RESULT[tree-with-fix]:    PASS
```

To A/B against this branch and the fix, check out
`repro/fallocate-replicas-test` in one tree and `fallocate-replicas` in another,
and point the script at both. Build products and logs land in `work/` beside the
script, or wherever `WORK` points.

## What failure looks like

Before the fix, the fallocated range is covered by a single
`KEY_TYPE_reservation` key spanning all 2048 sectors, and
`bch2_bkey_replicas()` returns 0 against a required 1:

```
key 1: KEY_TYPE_reservation  start=0 len=2048  nr_replicas(key)=1
       bch2_bkey_replicas() = 0   required = 1   -> FAIL
```

`bch2_bkey_replicas()` is written purely in terms of `bch2_bkey_ptrs_c()`, whose
`default:` case returns `{NULL, NULL}` for a reservation key, so the pointer
loop runs zero times. Its two immediate neighbours in the same file,
`bch2_bkey_nr_ptrs_allocated()` and `bch2_bkey_nr_ptrs_fully_allocated()`, both
special-case `KEY_TYPE_reservation` and return `v->nr_replicas`.

After the fix the same key reports 1 and the test passes, with nothing else
about the run changed.

## Two traps worth knowing about

Both cost real time to find, and neither is documented anywhere else.

**`linux_shrinkers_init()` must be called before anything else.** A C program
linking `libbcachefs` that skips it leaves `_totalram_pages` at 0, which makes
the journal's `mem_limit` 0, which pins the watermark at `reclaim`. Recovery and
fsck still succeed, so the filesystem looks healthy — but the first
normal-priority transaction then blocks on `journal_full` forever.

**Do not use a 512 MiB image.** At that size the journal buckets come out at
256 KiB, smaller than a single journal entry reservation, and the filesystem
wedges with `Journal stuck? ... journal_full` as soon as it goes read-write. The
runner uses 2 GiB.
