.. SPDX-License-Identifier: GPL-2.0

M3000 full-stack integration
===========================

This development branch combines M3000 v5, the bounded MSI/MSI-X stack
and the 15 deferred error-interrupt commits. It is a personal integration
branch, not a production qualification or a combined upstream submission.
The individual dated archive branches remain unchanged.

Dependencies
------------

The merge retains both histories:

* archive/2026-11-18/fujitsu/m3000-msi-bounded
  (7c9d6f5241d03911a343cf8f95e4b260c0d4efbb).
* archive/2026-11-18/fujitsu/m3000-error-interrupts-v5
  (e6d25a01d3a54f227ef177fb88bcf2991bc8c26f).

Both descend from M3000 v5 at
4542781ab46d822372ec5e3ce5a3ad680fcc6998, on v7.3-rc1.
The MSI parent includes the generic SPARC64 MSI lifecycle changes, tg3
reset/removal changes, BCM5718 byte-order fix, proposed PCI recovery API
and tg3 consumer. Other independent archive series are not implicitly
included; in particular this is not a PRIMEPOWER integration.

Only additive merge conflicts required resolution: include lists, error
snapshot sysfs setup and the documentation index. The MSI implementation
block matches the MSI parent, and the diagnostic/capture block matches
the error parent. MSI queues use INOs 0x18-0x3b; error capture expects
INO 0x3e. Disjoint numbers alone do not qualify concurrent operation.

Validation on 18 November 2026
-----------------------------

Runtime source commit: f7a2632f69239ed3c31a81d0ce48cc92cf169345.
Subsequent integration documentation does not change runtime code.

Both UP and SMP vmlinux builds passed with GCC 13 and W=1. The logs
contain kernel-doc and Fusion unused-variable warnings in files unchanged
from vanilla v7.3-rc1. No modpost section mismatch was reported.

Extracted-code tests passed under sanitizers:

* 683 shared-owner scenarios.
* 10 endpoint-mask/allocation rollback cases.
* 21 PCI notification/API cases.

These use mocked services; they do not replace kernel concurrency,
lockdep, runtime-PM, unbind or physical fault testing. The combined image
has not been boot-tested. No running kernel or GRUB configuration changed.

Limits and defaults
-------------------

Error diagnostics and capture remain opt-in. Do not infer that building
this branch enables error routes. Firmware ownership, fresh error delivery
and simultaneous live MSI/error recovery remain qualification gaps.
The historical RUC clear/ack/rearm controls are experimental, not a general
error policy. See oberon-error-interrupts.rst for the error-only lineage.

The MSI implementation still retains its bounded lifetime resources:
1 MiB per PBM and 256 lifetime IDs, with legacy fallback after exhaustion.
Queue reclamation is not implemented. Unsupported PM, CPU-hotplug and
kexec configurations retain legacy operation. See oberon-msi.rst for
API and hardware limits; integration does not remove those restrictions.

SMP image
~~~~~~~~~

Release: ``7.3.0-rc1-m3000-fullstack-20261118-smp``

vmlinux SHA-256::

  36efbd7b036ae0f6aab6c593bd01f46393bfe6c4c3523e7cd486bf3b215ce88d

UP image
~~~~~~~~

Release: ``7.3.0-rc1-m3000-fullstack-20261118-up``

vmlinux SHA-256::

  1943948c2df36e87533379eda6478e192abe9d3f0f87f80a09ca75cc17df0bfd
