SPARC32 and HyperSPARC archive
=============================

Local archive import: 18 November 2026. No GitHub push, builds, installations
or runtime tests were performed. Existing kernel branches were not changed.
This collection is suitable for the owner's personal GitHub archive; that
is distinct from Linux-maintainer submission readiness.

Start here
----------
* generic-v2/: latest nine-patch generic submission, 9 November 2026.
* bitlock-r7/: posted bit-lock fix, latest preserved prose split.
* hypersparc-fixes-r7/: nine HyperSPARC correctness fixes.
* optimizations-r7-held/: ten optimizations; experimental publication is
  allowed, but Linux submission is still held for outstanding review work.
* viking-demap-r8/: corrected demap companion retaining mm_cpumask.
* external-prerequisites/: mandatory PIL/IPI and full-stack PROT_NONE fixes.
* external-original-mail/kaloz-casa/: original Kaloz CASA/futex/divide mail.
* historical/generic-r7/: exact older dependency of the r7 HyperSPARC groups.
* integration-r5opt/: the tested full stack, split into functional folders.
* evidence/: preserved concise integration/validation records.

The JSON branch catalog records exact bases and tips. An archive branch can
include dependencies; BASE..TIP identifies only that entry's own commits.
Raw mail is authoritative for original exported messages, authors and
trailers. Reconstructed commits preserve authors and author dates where the
mail supplied them. At the owner's explicit request, missing Magnus
Lindholm sign-offs were appended to reconstructed commits, not raw files.
For the pasted IPI mail without a date, the import date is used and recorded
as such; no historical date, Message-ID or authorship is invented.

Version and dependency boundaries
---------------------------------
Generic v2 is based on the specifically selected unmerged local for-next
commit a9e88d617a25306fa7e532b0b8be08df778872cc (7.3.0-rc1), not current master
or a claimed v7.4-rc1 rebase. Its extra source-alias flushes passed local
SMP/UP build and QEMU tests; fresh physical validation remains pending.

The r7 HyperSPARC and optimization branches retain their older generic r7
and bit-lock dependencies on cee9395acd8043be0644b25c34bfa86623f2b935. They
have NOT been silently rebased onto generic v2. Generic r7 is archived as a
historical dependency, not offered as the latest submission.

The historical integrated full stack contains highload/HdrS 0x0300,
memblock, legacy CAS/HWCAP support, external SMP/vmalloc/seccomp changes,
bit-lock, generic and HyperSPARC fixes, integrated Viking support, the later
Kaloz CASA/futex/divide implementation and legacy trap bridge, the ASLR
experiment, and ten optimizations. Legacy atomic/divide patches in its
history are replaced later: do not select them as the final CASA solution.

Fullstack-r5opt-historical reconstructs the exact tree of the preserved
GDB-tested kernel. Fullstack-protnone1-historical adds the preserved Ramin
PROT_NONE fix and matches its hardware-tested source tree. Neither contains
the later generic-v2 source-alias revision. These are historical integration
snapshots, not a new claim of end-to-end validation of today's submissions.

The corrected demap companion differs from the September posted Viking v2.
Do not reintroduce its mm_cpumask shortcut. Viking instruction publication
and the HyperSPARC hook overlap; use the recorded integrated resolution
when reproducing that stack, not both standalone hooks blindly.

ASLR remains an experiment with limited entropy and preserved qualifications.
Its SPARC32 tests do not constitute runtime validation of Stian's SPARC64
patches. Old evidence README files may describe an intermediate state;
consult their dated evidence and the catalog rather than treating every
historical status sentence as current.

Reproduction and future development
-----------------------------------
Use recipes/sparc32-fullstack-r5opt.series for the exact 66-patch integration
from cee9395acd8043be0644b25c34bfa86623f2b935. The PROT_NONE recipe adds its
67th patch. Exclude cover letters from git am. The other series use the
BASE in catalog/sparc32-branches.json, not the current master tip.

Do not build, test or develop in this archive checkout. Start a new branch
in a separate clone/worktree elsewhere. Keep the dated archive refs fixed;
archive later work under a new dated/revisioned name. Future SPARC32 builds
still require the reviewed prerequisite gates and SS-20 foundation checks.
No checker was bypassed here: this import builds no kernel.

Codex assisted with inventory, mechanical replay, dependency records and
verification. No code fix or new runtime validation is claimed by import.
