Repeated COW/reuse tests

Primary test: cow-reuse-focused.c
  gcc -O2 -Wall -Wextra -Werror -o cow-reuse cow-reuse-focused.c
  ./cow-reuse 16 256

Optional harness self-test (must FAIL with exit 1):
  ./cow-reuse 2 256 --inject

Reserve 256 consecutive virtual base pages (1 MiB with 4 KiB pages).
Keep ONE page active for all reuse cycles, then advance to the next
virtual colour. This limits data-cache churn that could mask a stale
kernel alias. Every child checks the inherited pattern, writes one word
to force COW, then verifies that word and ALL untouched words. The parent
waits for child exit, verifies its original contents are unchanged, and
rewrites the entire original page with a new per-round pattern before the
next fork. Parent/child roles alternate between the first and last allowed
CPU, handling non-contiguous CPU IDs; UP uses the single CPU.

cow-reuse.c is the supplementary broad test: each child copies all pages
before exit. It adds multi-page coverage but can evict stale cache lines,
so the focused test is preferable for investigating this particular bug.
Both fail on data mismatch, abnormal child status or syscall failures.
External timeout is recommended for future hardware runs. Neither changes
system settings, requires root, nor intentionally faults or dumps cores.

Limits:
- The original page becomes eligible for wp_page_reuse; this userspace
  test does not instrument/prove that exact kernel branch or count it.
- Virtual offsets span colours; physical allocation/direct-map colour and
  lowmem residency are not controlled. A future HyperSPARC reproduction
  must verify lowmem allocation separately, or use an approved lowmem-only
  test setup. Do not claim all direct-map/user colour pairs were verified.
- No shared writable alias is created. MADV_NOHUGEPAGE prevents host THP
  from changing the base-page test (EINVAL accepted on unsupported kernels).
- Local host tests validate the harness. QEMU SS-20 uses a TI CPU model and
  does not validate HyperSPARC cache aliasing, nor show v1 fails/v2 passes.
- SS-20 was not accessed. No hardware binary was uploaded or run.

Results:
Host focused: 16 cycles x 256 colours = 4096 checked COW copies, PASS.
Host broad: 200 rounds x 256 pages = 51200 checked copies, PASS.
Both host injected-data controls: exit 1 and the expected mismatch, PASS.
QEMU SMP2 and UP1 focused: each 8 x 256 = 2048 checked copies, PASS.
QEMU SMP2 and UP1 compact broad: each 100 x 256 = 25600 copies, PASS.
Additional QEMU SMP2/UP1 focused runs at 128 MiB: each 2048 copies, PASS;
boot logs confirm 0K highmem, explicitly exercising lowmem source mappings.
This confirms lowmem test execution under QEMU, not HyperSPARC cache behavior.
Logs and command JSON files are kept here.

Initial broad initramfs contained two unstripped static binaries (~4 MiB
compressed). Both QEMU guests trapped during early boot before init/test.
Original logs, JSON, binaries and archive are retained. Stripping only the
binaries reduced the same broad archive to about 550 KiB and both guests
passed without changing the kernels or test logic. This isolates a harness
size/layout sensitivity; the precise early-boot defect was not diagnosed.
Use focused.cpio.gz or cow-reuse-compact.cpio.gz, not cow-reuse.cpio.gz.
