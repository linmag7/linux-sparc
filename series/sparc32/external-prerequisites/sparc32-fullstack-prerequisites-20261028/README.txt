External prerequisites for future SPARC32 full-stack builds (2026-10-28)

Retain the Imre PIL and Ramin observed-IPI prerequisites from the preserved
sparc32-build-prerequisites-20261018 directory. Add the byte-identical private
PROT_NONE patch here, authored by Ramin Moussavi and supplied via Imre.
Keep all three outside our submission series and retain their attribution.
Do not add a Tested-by trailer automatically or claim official publication.

Run verify-fullstack-prerequisites.py SOURCE before building and again before
packaging/installing, in addition to the applicable reviewed full-stack guard.
This wrapper invokes the original SMP checker and checks the reviewed
pte_present() predicate, including the COW-safe SRMMU_PRIV_RDONLY marker.
It is a source gate, not a replacement for runtime validation or review of
changed SRMMU definitions. Unknown/upstream-equivalent layouts require manual
review and a reviewed checker update. Do not bypass a failure.

Apply only if absent, in a new build tree. Never apply twice. Preserve all
existing releases, source trees and installed kernels. Tests and provenance
are linked in PROVENANCE.json. Existing full-stack guards remain required.
