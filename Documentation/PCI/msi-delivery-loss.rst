.. SPDX-License-Identifier: GPL-2.0

Permanent MSI delivery loss
===========================

This proposed interface handles permanent loss of a host controller's
MSI/MSI-X delivery service. It does not repair the controller or implement
PCI error recovery. A usable PCI function may still transfer data after
its interrupt delivery service fails.

Controller responsibilities
---------------------------

Before notification, contain the failed interrupt path and reject further
MSI/MSI-X allocations for every affected function, including new driver
bindings. Hold a device reference for each notification. Retain any queue
storage, message identifiers or mappings that may still receive writes;
notification results are not permission to reuse or free those resources.

Call from sleepable process context without device, driver, controller or
RTNL locks. Do not call from a PM callback, IRQ handler or error-recovery
callback. Neither helper supplies a timeout or cancels a stalled driver.
Release controller locks before notification: driver recovery may free
vectors and re-enter the controller's teardown path.

Call pci_msi_notify_lost() once per affected device. On failure, a caller
may use pci_msi_report_failure() to request a software availability change.
An error from either helper leaves recovery unresolved; do not describe
this as device quiescence. A controller must define its admission and
failure policy for drivers lacking either callback.

Recovery callback
-----------------

The core serializes against system sleep, obtains a runtime-PM reference,
and holds the PCI device lock while calling the current driver's
``msi_lost`` method. The device lock serializes driver unbind. The driver
must take its own operational locks to serialize ordinary I/O control.
Do not unbind the device or recursively acquire the PCI device lock.

Return zero after recovering service without MSI/MSI-X, one if no MSI
recovery was needed, or a negative errno. In particular, preserve an
administratively inactive device. A driver may stop and reopen its normal
I/O path using legacy interrupts if the device remains accessible. A
failed restart must leave a defined software state and report the error.

The core checks registration, disconnection, PCI channel state and D0
before dispatch, and rejects nonnegative results if MSI/MSI-X remains
enabled. These checks do not freeze asynchronous hardware state. Callers
must also verify their own vector ownership before accepting recovery.
The returned outcome describes this invocation, not a persistent health
state or proof of endpoint DMA completion.

Failure callback
----------------

The ``msi_lost_failed`` method runs under system-sleep serialization and
the PCI device lock, but without a runtime-PM resume. It must not initiate
hardware accesses or assume D0, PCI channel availability or successful
completion of an earlier recovery callback.

Inspect the current driver's state and return zero after marking an
affected MSI user unavailable, one if there is no affected MSI user, or a
negative errno. Repeated notification must be harmless. Retain resources
needed by outstanding work or DMA; a software availability change alone
does not stop either. Waiting for existing work does not retroactively
prevent hardware accesses already in progress.

The two helpers acquire and release the device lock independently. A
remove/rebind can occur between them, so the failure callback cannot
assume that this binding received the recovery callback. It must not mark
an unrelated, already-legacy binding unavailable merely because an older
binding failed recovery.

Relationship to PCI error recovery
----------------------------------

PCI error recovery coordinates channel errors and reset/resume callbacks
across affected devices. Permanent loss of an MSI delivery service does
not itself establish a channel error or require a PCI reset. These helpers
do not manufacture an AER event, change PCI channel state or override an
existing error-recovery owner. Whether this interface should be integrated
with existing PCI recovery facilities remains a subsystem review question.

Why notification is proposed
----------------------------

Blocking new MSI allocations does not convert an already-running device
to legacy interrupts. The driver owns its interrupt handlers, queues and
DMA resources, so it must participate in changing interrupt mode. Calling
pci_free_irq_vectors() directly from a controller failure worker would
bypass that ownership and the driver's teardown ordering.

Existing PCI error callbacks are not a drop-in conversion mechanism. For
example, tg3's slot-reset path restores PCI state and its resume path
restarts the existing device setup; they do not perform the normal close
and open that releases vectors and selects legacy interrupts. Pretending
that MSI delivery loss is a channel error would not supply that behavior.
An extension to PCI error recovery could provide equivalent semantics,
but would need to distinguish permanent interrupt-service loss from an
ordinary recoverable PCI channel fault.

A global notifier would still need binding lifetime, PM and driver locking
rules, while making driver participation harder to inspect. Forced unbind
would stop service and bypass the opportunity to recover an active device
through its own lifecycle. Neither is used by this proposal.

The recovery and failure methods have different access contracts. Recovery
may resume the device and perform normal hardware teardown/restart. Failure
notification can be required precisely because resume or recovery failed,
and must not initiate hardware accesses. Keeping those operations separate
makes driver opt-in explicit; an action-enumerated callback could express
the same contract if preferred during PCI subsystem review.
