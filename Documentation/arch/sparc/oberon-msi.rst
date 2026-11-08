.. SPDX-License-Identifier: GPL-2.0

Oberon MSI/MSI-X support
=======================

Scope
-----

The M3000 Oberon implementation provides one shared event queue per bridge.
Endpoints need a legacy IRQ and a driver implementing both permanent MSI
loss callbacks. Configurations with CPU hotplug, kexec or system sleep keep
legacy interrupts. The onboard BCM5718 ports are the qualified endpoints;
callback availability alone is not hardware qualification of other devices.

Normal allocation and teardown
------------------------------

An initialized bridge owns a 1 MiB coherent allocation containing its event
queue storage. Once published to hardware, the allocation, mapping, backing
device reference and queue handler are retained for the rest of the boot.
Pre-publication failures release their resources normally. This is one
allocation per initialized bridge, not one allocation per interface open.

Each vector consumes one of 256 message IDs. An ID is never assigned to a
second vector during the same boot, including after failed setup attempts.
Teardown invalidates its hardware map, removes software dispatch and waits
for queue-handler execution before releasing the Linux IRQ. The Linux IRQ
number can be reused; the retired hardware message ID cannot. Late records
for retired IDs have no dispatch target and cannot reach a new vector.

When the budget is exhausted, new allocations fail and a driver supporting
legacy fallback can select INTx. Existing vectors continue using MSI/MSI-X.
The budget is shared across all endpoints on the bridge and does not recover
on driver unbind, interface reopen or a function reset. Do not treat the
number of reopen cycles as an interface guarantee: vector counts and failed
allocation attempts affect consumption.

This policy trades repeated-allocation capacity for avoiding stale-message
aliasing. It does not provide unlimited MSI operation across rebinds. Legacy
fallback at exhaustion is intentional; removing the limit requires a proven
message-retirement protocol, not a larger software counter.

Failure policy
--------------

A queue failure permanently blocks new MSI allocations on that bridge,
invalidates message maps and disables queue delivery. Storage and the
handler remain retained. A worker notifies affected devices outside
controller locks, allowing drivers to release vectors and reopen on legacy
IRQs. A returned error does not skip other devices, but a callback that does
not return delays later notifications; there is no safe generic callback
cancellation mechanism.

If recovery fails, the software failure callback can mark an interface
unavailable while retaining its DMA resources. This does not prove endpoint
DMA or NAPI has stopped. No register readback, notification result or lack
of further interrupts is used as permission to reclaim the queue.

Conditions for future reclamation
---------------------------------

Queue storage reclamation and message-ID reuse are separate problems.
The implementation does neither. Reuse requires all of the following:

* Stop the old endpoint's MSI producer and establish the applicable PCI
  ordering boundary for messages already posted by that endpoint.
* Invalidate the map and establish that previously accepted messages have
  completed queue publication or will be discarded before a new mapping.
* Consume old records through that boundary and synchronize CPU consumers
  without resetting or discarding other endpoints' shared-queue records.
* Only then return the ID to the allocator.

Whole-queue reclamation additionally requires stopping every producer,
completing every possible DMA write and removing the published queue address
before freeing storage or the mapping. Synchronizing CPU IRQ handlers alone
does not provide that device-side completion boundary.

IDLE state, equal head/tail pointers and stable repeated reads are snapshots,
not a documented fence for all earlier endpoint writes. The EQWR_N latch is
cleared during consumption and is not treated as an independent completion
fence. A software generation counter cannot distinguish old and new uses
when the incoming record contains only the reused message ID. A fixed delay
or successful stress test is not a substitute for the ordering contract.

Unsupported extensions
----------------------

Queue reclamation, ID reuse, per-vector queue affinity and suspend/resume
are not implemented. Actual PCIe faults, runtime PM, AER overlap, hot removal
and other endpoint drivers require further qualification. Bridge error
interrupt handling is outside this MSI/MSI-X implementation.
