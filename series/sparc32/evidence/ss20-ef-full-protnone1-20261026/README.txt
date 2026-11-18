Full-stack PROT_NONE candidate -- 26 October 2026

Kernel: 7.3.0-rc1-ef-full-protnone1+
Proposed separate SILO label: ef-full-protnone1 (not installed).
Foundation: successful r5opt GDB-build kernel, including all ten
optimizations, current bit-lock fix, Viking/HyperSPARC integration,
CASA/futex ABI, division emulation, highload/memblock, native ASLR,
seccomp/vmalloc fixes and both mandatory SMP prerequisites.
Only new source patch: Ramin Moussavi's PAGE_NONE presence fix, unchanged.
Preserve attribution; keep separate from the public E/F submissions.

See REVIEW.txt for the semantic audit, VALIDATION.txt for results, and
package/MANIFEST.json for image and test hashes. Build and UP image remain
available; package contains the self-contained SMP image and test tool.
Hardware installation and 200-round test remain pending. The original
source/test inputs and failed QEMU attempts are preserved.
