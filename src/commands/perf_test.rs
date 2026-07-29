// perf_test: run the in-tree btree performance/stress tests from userspace.
//
// bch2_btree_perf_test() (fs/debug/tests.rs) has existed for a long time but has
// only ever been reachable through the kernel's `perf_test` sysfs attribute
// (fs/debug/sysfs.c). The code itself is dual-build and the symbol is already
// linked into the userspace binary — there was simply no way to call it without
// a kernel.
//
// That matters for more than convenience. These tests are the only ready-made
// way to drive a *long* btree scan and a concurrent mixed workload against the
// same filesystem, which is what you need to measure btree lock-hold and lock
// starvation behaviour. In userspace they run under valgrind, ASAN, gdb, perf
// and rr, and they iterate in seconds rather than a kernel build plus a VM boot.
//
// The filesystem is opened read-write: every test except the lookup-only ones
// inserts, overwrites or deletes keys, and they all operate on scratch ranges of
// the extents and xattrs btrees. Point this at a scratch filesystem, not at
// anything you care about.

use std::ffi::CString;
use std::path::PathBuf;

use anyhow::{bail, Result};
use bcachefs_kernel::c;
use clap::Parser;

/// Run the in-tree btree perf/stress tests
#[derive(Parser, Debug)]
#[command(about = "Run btree performance tests (debugging)")]
pub struct PerfTestCli {
    /// Test to run, e.g. seq_insert, seq_lookup, rand_lookup, rand_mixed
    #[arg(long)]
    test: String,

    /// Number of iterations, split across threads
    #[arg(long, default_value_t = 1_000_000)]
    nr: u64,

    /// Number of concurrent threads
    #[arg(long, default_value_t = 1)]
    threads: u32,

    /// Device(s)
    #[arg(required = true)]
    devices: Vec<PathBuf>,
}

fn cmd_perf_test(cli: PerfTestCli) -> Result<()> {
    if cli.nr == 0 || cli.threads == 0 {
        bail!("--nr and --threads must both be nonzero");
    }

    let fs_opts = c::bch_opts::default();
    let fs = crate::device_scan::open_scan(&cli.devices, fs_opts)?;

    let name = CString::new(cli.test.as_str())?;

    // Safety: fs.raw is a live bch_fs for the duration of this call, and `name`
    // outlives it. bch2_btree_perf_test takes both by borrow and does not retain
    // either past return.
    let ret = unsafe {
        bcachefs_kernel::debug::tests::bch2_btree_perf_test(
            fs.raw,
            name.as_ptr(),
            cli.nr,
            cli.threads,
        )
    };

    if ret != 0 {
        bail!("{} failed: {}", cli.test, ret);
    }

    Ok(())
}

pub const CMD: super::CmdDef =
    typed_cmd!("perf-test", "Run btree performance tests", PerfTestCli, cmd_perf_test);
