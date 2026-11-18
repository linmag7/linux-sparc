.. SPDX-License-Identifier: GPL-2.0

M3000 Oberon error-interrupt development archive
===============================================

Status and provenance
---------------------

This is experimental work published in Magnus Lindholm's personal kernel
repository. It is not a claim of acceptance by Linux maintainers, complete
fault recovery or production readiness. The branch retains the exact first
15 commits from the historical M3000 hardware export of 2026-10-16, followed
by this documentation commit. Original commit IDs, authors and sign-offs
are preserved. Historical MSI patches 16 and 17 are deliberately excluded.

Dependencies
------------

The exact parent is the ten-patch M3000 v4 tip:

  0bce9f9cf842826de2dc674cca3008d2b38fd85b

That base is built on vanilla v7.3-rc1:

  cee9395acd8043be0644b25c34bfa86623f2b935

The last error-interrupt implementation commit is:

  6e75fedce50a17808e9737c629f8b630d0d7f4b8

This branch is not rebased onto M3000 v5 and is not an add-on to the current
bounded MSI branch. Both modify Oberon code; combining them requires an
explicit integration review and new qualification. Generic PCI recovery
APIs, tg3 MSI recovery and the generic MSI series are not source-level
prerequisites for this historical error-only subset. The standalone tg3
BCM5718 byte-order fix is separately needed for the tested M3000 network
setup. Include relevant generic fixes when preparing a new test image.

Implemented experiments
-----------------------

* Opt-in passive MMU/PEC snapshots and bounded fault/header capture.
* A prepared but disabled capture-once route, with explicit live controls.
* Capture containment, a captured-RUC clear experiment, masked
  acknowledgment and one bounded rearm generation.
* Masked route-target selection, late-capture retention and a terminal
  containment-failure panic path.

The boot options include ``oberon_diag``, ``oberon_capture_prepare`` and
``oberon_capture_arm_port``. They and the private sysfs controls are
laboratory interfaces, not a supported stable ABI. Route preparation and
selection must not be confused with proof of interrupt delivery to a new
CPU. This archive does not turn the experiments on by default or modify
any machine's boot configuration.

Remaining limitations
---------------------

* Only captured RUC has a bounded clear/acknowledgment experiment. Arbitrary
  errors, overflow recovery and general fault-recovery policy remain open.
* Firmware/XSCF ownership and handoff are not a general solved contract.
  Do not assume all status/clear registers can be safely accessed or owned
  by Linux because one inherited error was captured successfully.
* Fresh delivery to a newly selected CPU remains unproven in this export.
* Private controls, IRQ registration and lifetime management still need
  cleanup before a maintainable general backend can be claimed.
* Reboot/kexec handoff and full recovery are not established. The terminal
  panic path is containment, not a recovery guarantee.
* Register readback and CPU IRQ synchronization do not establish completion
  of all device DMA. This work supplies no MSI queue-reclamation proof.

Testing evidence and limits
---------------------------

The original export records successful UP/SMP W=1 builds at every hardware
prefix, including the first 15 commits. That is historical build evidence,
not a new build performed for this archive; it is not warning-free evidence.

Earlier M3000 test images captured inherited RUC, cleared its source and
summary and acknowledged a masked route. Those are historical results, not
an exact standalone-subset boot claim. The original combined Ultra45 boot
checks coexistence on Fire, not Oberon error handling. The independent
error-only branch has not been newly booted or fault-injected here.

Archive preparation verified original patch IDs, exact implementation tree
and sign-offs. The only newly added kernel-tree changes are documentation.
Future tests should use a separate worktree and explicit recovery access;
keep this dated branch as an immutable record of the original experiments.
