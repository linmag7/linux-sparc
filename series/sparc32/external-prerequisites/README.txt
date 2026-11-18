External SPARC32 build prerequisites -- copied 18 November 2026

Preserved external patches, separate from Magnus's submission series:

1. Imre Kaloz: sparc32: keep PIL raised on the SMP ticker and LEON IPI
   exit paths. Message-ID: 20260927005658.8242-1-kaloz@kernel.org
   Required by local policy for every future SPARC32 kernel build.

2. Ramin Moussavi: sparc32: sun4m: do not clear IPIs that were not handled.
   Required by local policy for every future SPARC32 kernel build.
   The preserved mail wrapper was reconstructed from supplied mail text;
   no Message-ID was supplied in that record.

3. Ramin Moussavi: sparc32: treat PAGE_NONE ptes as present.
   Required by local policy for future SPARC32 full-stack builds.
   Supplied privately via Imre; original attribution and trailers retained.
   No official posting or Message-ID was verified in the preserved record.

These are the preserved prerequisite versions, not a fresh upstream-status
check. Do not infer current upstream inclusion or publication status from
these records. No new Tested-by or Reviewed-by trailer was added.

Application order
-----------------
The series file lists 1, 2, 3 in the standard build order, with paths relative
to this directory. Review the target tree first. Skip changes already
present or manually verified upstream equivalents; never apply twice.
The three fixes address distinct defects. This ordering is the local
integration convention, not a claim that each logically depends on the
preceding patch. PIL and IPI both modify entry.S; review conflicts rather
than forcing application on a changed layout.

For a fresh tree where all three changes are absent, from that tree:

  prereqs=/home/lindholm/sparc32-patches/00-external-prerequisites
  while IFS= read -r patch; do
      git am "$prereqs/$patch" || break
  done < "$prereqs/series"

Use this only after checking applicability and existing equivalents.
No application/build was performed as part of this copy operation.

Relationship to the other folders
--------------------------------
These are runtime/build prerequisites in the tested integration, not
submission dependencies that must be folded into the generic patch mails.
The nine generic patches apply independently on their recorded base.
Their local validation kernels included fixes 1 and 2; the PROT_NONE rule
is specifically a full-stack build requirement.

A conservative integration sequence, after reviewing the base, is:
  * Required SS-20 platform foundation, if building for that machine.
  * Missing external prerequisites 1, 2, 3 above.
  * 01-bitlock (independent fix, included in the combined integration).
  * 02-generic-v2, following its series file.
  * 03-hypersparc-fixes, following its series file.
  * 04-optimizations-HELD only for an explicitly requested test build.

This is a dependency outline, NOT a newly replay-validated combined stack.
The copied series have different recorded bases. Reconcile/rebase and run
checks before building. Generic v2 still awaits new hardware validation.

05-viking-demap is a separate corrected companion. The earlier generic
revision and this companion were checked in both application orders; that
is not a fresh combined-stack validation with generic v2. Keep its removal
of mm_cpumask narrowing. Do not substitute the previously posted Viking v2
unchanged: it retains the problematic shortcut. The separate Viking
instruction-update patch overlaps the HyperSPARC instruction-cache hook
and needs the reviewed combined implementation when integrating both.

SS-20 full-stack foundation
--------------------------
These three patches alone do not make a hardware-ready SS-20 kernel.
Retain highload v3 / HdrS 0x0300, memblock, the existing CAS/futex trap ABI
and HWCAP, UDIV/SDIV emulation and preserved platform support. These are
not copied into this three-fix prerequisite bundle or folded into the
submission series. Historical foundation/integration references:

  /media/lindholm/545a6254-41a9-48f3-a207-cbea64457d45/linux/ss20-ef-fullstack-ipi2-20261018/FOUNDATION.txt
  /media/lindholm/545a6254-41a9-48f3-a207-cbea64457d45/linux/ss20-ef-fullstack-ipi2-20261018/INTEGRATED-ORDER

That October integration includes older revisions; do not blindly replay
its entire patch list over these newer submission revisions. Other tested
external additions (for example vmalloc/seccomp) are separate integration
components, not extra patches silently included here.

Verification
------------
Directory names are retained so the original checker imports resolve.
For the two mandatory SMP fixes:

  python3 "$prereqs/sparc32-build-prerequisites-20261018/verify-sparc32-prerequisites.py" SOURCE

For all three full-stack prerequisites:

  python3 "$prereqs/sparc32-fullstack-prerequisites-20261028/verify-fullstack-prerequisites.py" SOURCE

Run before building and again before packaging/installing a full-stack
kernel, alongside its applicable reviewed SS-20 full-stack guard. These
read-only checks recognize reviewed source layouts, not every possible
upstream equivalent. A failure requires manual equivalent-code review and
a reviewed checker update, never bypassing the check. Keep the existing
full-stack guard and packaged-image highload-header check.

COPY-MANIFEST.json records byte-identical source provenance. SHA256SUMS
covers the copied bundle. Original source bundles, release directories,
installed kernels and the SS-20 were left unchanged.
