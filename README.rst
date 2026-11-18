SPARC64 hardware-support archive
===============================

This branch is the canonical collection of original patch mail files,
cover letters and a dependency catalog, snapshotted on 2026-11-18.
It is not a kernel source tree or a build workspace. Kernel source lives
on the dated archive branches listed below. master and the existing category
branches remain unchanged. Base for these series: Linux v7.3-rc1
(cee9395acd8043be0644b25c34bfa86623f2b935), not current master.

Navigation
----------
* series/fujitsu/: M3000 v5, bounded Oberon MSI and PRIMEPOWER support.
* series/sparc64/: consolidated generic fixes, extra fixes and MSI lifecycle.
* series/drivers/, series/pci/, series/serial/: independent subsystem work.
* series/workarounds/: SPI domain-validation workaround.
* series/historical/: deferred combined hardware development export.
* catalog/branches.json: exact branch bases, tips, dependencies and status.
* catalog/commit-map.json: original exported commit -> reconstructed commit.
* SHA256SUMS: original patch and cover-letter checksums.
* recipes/: integration orders; exclude cover letters from git am.

Archive policy
--------------
Original mail files are byte-for-byte copies, including their historical
messages and trailers. Reconstructed kernel commits preserve author, author
date and mail commit message; committer metadata differs, so commit IDs can
differ. Each reconstructed diff was checked against its stable patch ID. Where
dependency context changes that ID, reverse-applying the original diff was
required to recover the exact parent tree; the catalog records both IDs.
Raw mail files are authoritative for exact original exports and cover letters.
Codex assisted with collection, mechanical replay and documentation.

Dated archive branches are frozen snapshots: do not rebase or force-push
them. For future changes, create new topic branches and worktrees elsewhere,
then add another dated archive snapshot and update this catalog. Do not
build in this repository's checked-out master. This import performs no new
kernel build or runtime test and makes no new upstream acceptance claim.

Dependencies and exclusions
---------------------------
M3000 and PRIMEPOWER are separate dependency stacks, not one combined
mailing. The PRIMEPOWER branch includes its documented generic, serial and
driver prerequisites. The Oberon branch includes generic MSI, tg3 reset,
byte-order, PCI notification and tg3 recovery prerequisites. See branch
base and tip to identify each series' own commits independently of its stack.

The original four-patch extra-fixes export is retained unchanged, but only
patches 1, 3 and 4 are replayed. Patch 2 is superseded by consolidated generic
patches 2 and 3. Do not apply both. The 17-patch historical hardware export
mixes deferred error handling and superseded MSI; it is not replayed or
stacked on the new Oberon series. Error interrupts remain deferred.

The current MSI proposal has a finite 256-ID budget and retains published
queue memory. It remains an RFC, requiring PCI API and retention-policy
review; it is not unrestricted production lifecycle support. PRIMEPOWER
remains an RFC with restricted IRQ migration. SPI DV remains a workaround.

Kernel branches
---------------

archive/2026-11-18/fujitsu/m3000-v5
  Held local v5 candidate; 10 own patches.

archive/2026-11-18/sparc64/common-review69
  Consolidated generic fixes and API preparation; 9 own patches.

archive/2026-11-18/sparc64/extra-fixes
  Retained non-overlapping fixes; original patch 2 excluded; 3 own patches.

archive/2026-11-18/sparc64/msi-lifecycle
  Generic MSI lifecycle review series; 8 own patches.

archive/2026-11-18/sparc64/m3000-companion
  M3000-dependent common IRQ diagnostics; 1 own patches.

archive/2026-11-18/serial/sunsu
  Serial review series; 3 own patches.

archive/2026-11-18/drivers/sunhme
  Independent driver patch; 1 own patches.

archive/2026-11-18/drivers/tg3-fujitsu
  Independent driver patch; 1 own patches.

archive/2026-11-18/drivers/mptfusion-endian
  Independent driver patch; 1 own patches.

archive/2026-11-18/drivers/mptfusion-initiator-id
  Independent driver patch; 1 own patches.

archive/2026-11-18/drivers/tg3-bcm5718-byteorder
  Independent driver patch; 1 own patches.

archive/2026-11-18/drivers/pci-uli-ehci
  Independent driver patch; 1 own patches.

archive/2026-11-18/workarounds/spi-dv-workaround
  Workaround, root cause unresolved; 1 own patches.

archive/2026-11-18/drivers/tg3-reset-fixes
  Four generic reset/removal fixes; 4 own patches.

archive/2026-11-18/pci/msi-loss-api
  Proposed PCI API; RFC; 1 own patches.

archive/2026-11-18/drivers/tg3-msi-recovery
  Driver consumer RFC; 1 own patches.

archive/2026-11-18/fujitsu/primepower250-review69
  Platform RFC; restricted IRQ migration; 3 own patches.

archive/2026-11-18/fujitsu/m3000-msi-bounded
  Bounded RFC; 256 lifetime IDs; error interrupts deferred; 1 own patches.

Working with the archive
-----------------------
Use git show patch-archive:catalog/branches.json for the branch inventory.
Use git log BASE..TIP to review only a series' own commits. When creating
worktrees for future development, use a new branch, not a dated archive ref.
To extract the mail archive without checking out this branch:

  git archive patch-archive | tar -x -C /path/to/empty/export

GitHub publication uses explicit new refs only; master is not replaced.
