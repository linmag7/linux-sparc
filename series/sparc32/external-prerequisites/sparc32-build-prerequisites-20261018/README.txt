Standard external SPARC32 build prerequisites, 2026-10-18

1. Imre Kaloz: keep PIL raised on SMP ticker and LEON IPI exit paths.
   Message-ID: 20260927005658.8242-1-kaloz@kernel.org
2. Ramin Moussavi: sun4m: do not clear IPIs that were not handled.
   Received as user-pasted mailing-list text; Message-ID not yet supplied.

Keep both outside our E/F submission series, preserving author attribution.
Before every SPARC32 kernel build, run verify-sparc32-prerequisites.py SOURCE.
Apply missing changes only to new build sources. Never double-apply an existing
fix or verified upstream equivalent. The guard recognizes the reviewed entry.S
layout; changed upstream layout requires manual equivalent-code review.
Preserved diag5/diag6 and earlier release sources/images remain unchanged.
No hardware test or Tested-by claim is implied by prerequisite status.
