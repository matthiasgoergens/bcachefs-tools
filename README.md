# Reproducers for koverstreet/bcachefs-tools#803

Per-snapshot accounting keys written before 2026-07-16 are silently
reinterpreted by current code, because the new
`{id, btree} -> [nr_keys, key_bytes, external_sectors]` key aliases exactly onto
the old `{id} -> [sectors]` one — `btree` reads back as 0 (`extents`) from the
old key's zero padding, so the bpos is unchanged.

This branch holds the scripts referenced from that issue. It is a standalone
orphan branch: no bcachefs-tools source, just the reproducers.

Everything here runs **unprivileged on plain image files** — no loop devices, no
kernel bcachefs module, no VM. Two of them use `bcachefs fusemount`; the rest
use `bcachefs image update --keep-alloc` as the read-write open, which needs no
FUSE at all.

## What each one shows

| script | shows |
|---|---|
| `repro.sh` | the aliasing itself, and deletions driving counters negative. Two arms: A minimal, B full |
| `repro-accounting-underflow.sh` | first version, via `fusemount` — kept because it shows the aliasing without `image update` |
| `repro-accounting-underflow-v2.sh` | the minimal trigger: queued `background_compression` reconcile work, no file activity at all |
| `repro-snapshot-check-no-data.sh` | `bch2_snapshot_node_check_no_data()` failing **open** — a snapshot node deleted with a live key still accounted to it |
| `repro-trust-keys-during-upgrade.sh` | `trust_keys` is false during the upgrade's own recovery run, so the guard is disarmed exactly when `delete_dead_snapshots` runs |
| `repro-snapshot-data-sectors.sh` | the same defect in `snapshot_data_sectors()`, clearing a dangling child pointer that still has data behind it |

`instrumentation-2026-07-29.patch` is the ~77-line `bch_err()` diff against
`472b4aa4488e` used to print raw counter values. Two of the scripts are
confirmed both with it and with a stock binary on behaviour alone.

## Running them

Each needs two `bcachefs` binaries, one from either side of 2026-07-16:

```
OLD=/path/to/old/bcachefs NEW=/path/to/new/bcachefs ./repro.sh
```

`Dockerfile` builds both from source at pinned commits, so nothing depends on
binaries you happen to have:

```
OLD_REF  2c7d916a53b1ebc47ce6314146de10ab96ebf869   parent of e80608bb6
NEW_REF  472b4aa4488e0d3aee6041d033e24dc3b6af2852   master
```

```
podman build --tag bcachefs-acct-repro .
podman run --rm bcachefs-acct-repro
```

Override `--build-arg NEW_REF=<sha>` to bisect; earlier layers stay cached.

## A caveat worth reading

`repro-snapshot-check-no-data.sh`, `repro-trust-keys-during-upgrade.sh` and
`repro-snapshot-data-sectors.sh` **inject** accounting keys with
`bcachefs kvdb set accounting` rather than growing them naturally. For the
snapshot cases that is necessary rather than convenient: a real stranded xattr
is reaped by `check_xattrs` — which the upgrade *does* schedule — before
`delete_dead_snapshots` runs, which destroys the signal. An injected key in an
unchecked btree is precisely what an upgraded old filesystem looks like.

The injected layouts are not guesswork: a filesystem formatted by the real
`v1.38.8` binary yields `u64s 6 ... snapshot id=4294967295 btree=extents 512`,
and the injection reproduces that byte for byte.
