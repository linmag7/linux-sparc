Native SPARC32 ASLR experiment on the full SS-20 stack
=====================================================

Status: installed and independently verified; waiting for the user to boot it.
The ASLR kernel has not yet been booted or tested on HyperSPARC.
Kernel: 7.3.0-rc1-ef-full-aslr1
Installed separate SILO label: ef-full-aslr1.
Default remains ef-full-ipi2; no reboot was performed.

Source and scope
----------------
Base: ss20-ef-fullstack-ipi2-20261018, commit
03f9a70fb87ae067dcfc30aa4ef9e703b7c36772.
Final experimental source:
1ff129cee36049d6c382f93bf04b2f9594342daa.

The three original SPARC64 patches from Stian Halseth were applied with
attribution intact, followed by one separate native SPARC32 experiment.
Original cover Message-ID: 20261018122734.2594904-1-stian@itx.no.
Native tests do not constitute runtime validation of the SPARC64 series.
Patches are in patches/. The supplied messages and application-only audit
are preserved next door in sparc-aslr-v2-review-20261018/.

The new source retains the full highload v3/HdrS 0x0300, memblock,
CAS/futex/HWCAP, UDIV/SDIV, platform, seccomp/vmalloc, E/F rev2, Imre PIL,
and Ramin observed-IPI acknowledgement stack. Existing full-stack and
preserved diag5/diag6 sources, builds and installed files were not changed.
No diagnostics or cache-flush policy changes were added in this experiment.

Native behavior
---------------
Unhinted mmap starts at a randomized mm->mmap_base; the bottom-up search,
explicit hints, file-offset colour alignment and fixed-map rules remain.
PIE moves from 0x50000000 to 0x40000000 to separate it from mmap. This base
change also applies with ASLR disabled: disabled layouts are deterministic,
but are not byte-for-byte identical to the baseline PIE layout.

The mmap floor has an 8 MiB page-granular random offset. The PIE hook masks
that offset to SHMLBA boundaries, because fixed ELF mappings bypass normal
mmap colour selection. This preserves each ELF file page's prior colour
across executions; it does not repair pre-existing segment aliases or
arbitrary fixed mappings. With the SS-20's 512 KiB VAC this gives only 16
PIE offsets (four bits), reduced further by larger ELF p_align. It is a
conservative hardware experiment, not release-ready ASLR hardening. A larger
range/entropy policy needs separate review before proposing it upstream.

brk uses a page-granular window of up to 32 MiB, bounded below the user
address limit. CONFIG_COMPAT_BRK=y remains, so the default ASLR setting is
1; independent brk randomization requires randomize_va_space=2. Explicit
loader execution can still have an adjacent VMA block heap growth in mode
0 or 1 with this configuration. These observed cases are retained, not
claimed fixed. Mode 2 gave independent brk variation in the tested cases.

Build and prerequisites
-----------------------
The complete kernel and 62 modules built without compiler warnings/errors.
The package contains 68 hashed payload files, checked against its archive.
The compiled image advertises HdrS 0x0300. CAS/futex symbols and the compiled
IPI acknowledgement instructions were verified. BUILD.json and logs/ hold
the exact identities and checks. The source guard was copied for this
experiment and reviewed: only the exact ELF_ET_DYN_BASE replacement is
normalized for its comparison with diag6; every other critical foundation
byte remains required. Both mandatory SMP checks still run unchanged.

Package: hardware/ss20-full-aslr1.tar.gz
Image SHA256:
6ebf7d81aea2e358849b45d60fa3cfc80f631a0cfa91f0ab5c9ac90452186e86
Package SHA256:
0d829b2709313135f64f4c8032a9d45ce8abcce1f485e346b2bc27328b059f4d

Reviewed QEMU results
---------------------
The final matrix used revision5 tools, the cross-toolchain's glibc 2.19,
QEMU SS-20/TI SuperSPARC, 384 MiB, and two CPUs. It covered ASLR 0/1/2,
ADDR_NO_RANDOMIZE off/on, and five invocation types: ET_EXEC, ordinary PIE,
PIE with 8 MiB p_align, explicit loader plus ET_EXEC, and explicit loader
plus PIE. Sixteen repeated executions per group gave 30 groups.

Candidate: all 480 executions and 480 associated COW child checks passed.
All disabled layouts were repeatable. With ASLR enabled, library/mmap
addresses moved; ordinary PIE moved; brk varied independently in mode 2.
The 8 MiB-aligned PIE loaded successfully but its alignment consumed the
8 MiB PIE randomization range, as expected. Tests also checked mapped-file
pwrite visibility, shared/private aliases, COW isolation, caller-owned fixed
mappings, libm loading/calls and heap growth where existing VMAs permitted.

Baseline: 384 successful executions in 24 groups; all six large-alignment
PIE groups failed at dlopen with 'dlopen: invalid caller'. Failed groups
stop after their first failed probe (390 attempts total), then the matrix
continues. The same six groups passed on the candidate. This is evidence
for improved layout separation with this test/runtime, not a complete
root-cause analysis of the old loader. Baseline PIE heap growth was also
blocked by a pre-existing loader mapping in the observed cases.

Additional candidate checks passed: two-CPU shared CAS (4000 increments),
futex atomic add (100), 64 checked forks, and four-CPU IPI wakeup stress
for 60.067 seconds. All 16 IPI workers progressed; maximum sampled gap was
223 ms. Whole successful logs were reviewed with no panic, oops or RCU
stall. QEMU/OpenBIOS startup notices and intentional lack of a NIC peer
are retained in the raw logs.

The raw matrix review independently checked every final record and group.
One kernel 'random: crng init done' line interleaved with output in each
matrix log; the review removes exactly that line to join the interrupted
record. Raw logs remain unchanged. See tests/REVIEW.json and
scripts/review-qemu.py for the precise review and all group results.

Earlier attempts are preserved. An oversized debug-bearing initrd made
both kernels abort before tests; stripped copies booted. The precise early
boot failure mechanism is unresolved. Initial harness versions treated
legitimate brk refusal as a fatal probe error; later versions use the raw
syscall and require a visible conflicting VMA before classifying refusal.
These iterations are not counted as successful matrix coverage.

Hardware boundary
-----------------
QEMU does not validate HyperSPARC cache coherence. The 512 KiB colour path,
concurrent PIE execution on CPUs 0 and 2, current Gentoo libc, and a real
build still require SS-20 testing. No Tested-by tag is justified yet for
this native adaptation or for Stian's original SPARC64 series.

A read-only SSH check during preparation found the SS-20 still on
7.3.0-rc1-ef-full-ipi2, boot 4f5e3145-c0da-4c06-99d9-bf156d43678e,
taint 0, randomize_va_space=1, with no matching emerge/make/cc1/GDB/stress
processes in that snapshot. No hardware test, sysctl change, install or
reboot was performed. See HARDWARE-PLAN.txt for the remaining work.

Installation, 2026-10-18
------------------------
Installed 7.3.0-rc1-ef-full-aslr1 under the separate ef-full-aslr1 label.
The SILO change only appends its stanza; the default remains ef-full-ipi2.
Root stays /dev/sda4 with sysrq_always_enabled=1 and full memory.
No reboot or ASLR hardware test was performed. The current boot remains
311123b5-b97d-4210-8803-85833f968463 on ef-full-ipi2.

All 68 new payload files matched the package, including 62 modules. The
compressed image round-tripped to the QEMU-tested zImage and HdrS 0x0300.
The saved before/after log and 1206-entry core inventory are unchanged.
The original installer also verified 1756 older files unchanged. The user
subsequently clarified that blanket checks of existing module trees are
unnecessary: this kernel boots without modules. Future installs omit those
scans and verify the new image, SILO and files actually changed.

To make room in /boot, two older System.map files were copied and verified
on the root filesystem, with symlinks preserving their original paths.
The new map/config use the same metadata directory. No boot images were
deleted, moved or replaced. Exact paths and hashes are in
hardware/install-20261018/REVIEW.json and its preserved records.
SILO backup: /boot/silo.conf.before-ef-full-aslr1-20261018
Evidence archive SHA256:
b79c2e4d2f3d98bccc78b3f4f3acd0d23915b938789db1ee9e8671a6a6123744
