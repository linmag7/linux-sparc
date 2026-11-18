Separate companion: SuperSPARC demap serialization

Apply the single patch before or after submission-generic/series. Either
order gives the identical source tree, as recorded in ../merge-order.json.
The generic series does not require this new wrapper to retain mm_cpumask:
it fixes the existing generic wrapper, while this companion independently
keeps the accumulated mask in each new Viking wrapper. Do not import the
older mask-narrowing Viking variant. The companion serializes complete
shootdowns at the initiator and uses synchronous singleton cross-calls
with lock-free local backends. Dispatch is sun4m + Viking only; sun4d
retains its existing broadcast-demap protocol.

The separate Viking instruction-cache publication patch is excluded.
It overlaps the HyperSPARC-specific instruction update work, not these
nine generic changes. Keep that combined implementation for the later
HyperSPARC submission/integration.

Physical SuperSPARC validation remains pending. QEMU does not establish
real Viking demap timing or HyperSPARC cache coherence. Existing HyperSPARC
GDB success cannot exercise the Viking-only path. Do not claim otherwise.
