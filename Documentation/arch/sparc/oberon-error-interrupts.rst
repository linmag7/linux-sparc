.. SPDX-License-Identifier: GPL-2.0

M3000 v5 error-interrupt development branch
=========================================

Status
------

Experimental work published in Magnus Lindholm's personal kernel repository.
This branch ports the 15 archived Oberon error-interrupt commits onto the
latest archived M3000 v5 basic-support series. It does not claim production
readiness or submission/acceptance by Linux subsystem maintainers.

Dependencies and lineage
------------------------

Direct base: ``archive/2026-11-18/fujitsu/m3000-v5``

  4542781ab46d822372ec5e3ce5a3ad680fcc6998

That series is based on vanilla v7.3-rc1:

  cee9395acd8043be0644b25c34bfa86623f2b935

The original v4 error implementation ends at:

  6e75fedce50a17808e9737c629f8b630d0d7f4b8

Its original v4 branch remains unchanged:
``archive/2026-11-18/fujitsu/m3000-error-interrupts-experimental``.
Only its first 15 implementation commits were ported. Superseded historical
MSI patches 16-17 are excluded. This branch does not include the new bounded
MSI series, the proposed PCI recovery API or its tg3 consumer. Those are
not prerequisites for the error-only implementation. Combining the current
MSI and error branches still requires explicit integration and testing.
The standalone BCM5718 byte-order fix remains a separate network prerequisite
when preparing M3000 test images, as do other applicable generic fixes.

Port validation
---------------

All 15 commits applied directly without conflict resolution. Their stable
patch IDs match the original commits. Author identity, author date, commit
messages and Magnus Lindholm's Signed-off-by were preserved.

The v4-to-v5 foundation changes and the error series touch disjoint files.
The resulting error implementation files are byte-identical to the original
error-only tip. All v5 foundation changes are retained unchanged: the full
binary diff between original and ported error trees equals the v4-to-v5
foundation diff. ``git diff --check`` passed.

The only additional commit documents this branch and adds this page to the
SPARC documentation index. No build, boot or hardware fault test was run for
this port. Earlier UP/SMP build and M3000 RUC capture/clear/ack results apply
to historical images, not this exact v5 stack. The original combined Ultra45
test is evidence of Fire coexistence, not Oberon error recovery.

Implemented experiments and limits
----------------------------------

The series contains opt-in passive MMU/PEC snapshots, capture-once routing,
bounded capture/containment, captured-RUC clearing, masked acknowledgment,
one bounded rearm generation, masked target selection and late-capture
retention. Terminal containment failure can panic; this is not a recovery
guarantee.

``oberon_diag``, ``oberon_capture_prepare``, ``oberon_capture_arm_port`` and
the private sysfs controls remain laboratory interfaces, not a stable ABI.
This port does not activate error routes by default or change machine boot
settings. Preparation and target readback do not prove delivery to a new CPU.

Remaining work includes general firmware/XSCF ownership, full recovery for
arbitrary faults, overflow handling, IRQ registration/lifetime cleanup and
reboot/kexec handoff. Fresh error delivery to a newly selected CPU is not
proven by this export. Only captured RUC has a bounded clear/ack experiment.
Register readback and CPU IRQ synchronization do not prove device-DMA
completion or safe MSI queue reclamation.

Keep this dated archive branch intact. Use separate development branches and
worktrees for integration with MSI, new builds and hardware qualification.
