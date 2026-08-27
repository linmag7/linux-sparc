=================================
The sparc32 compare-and-swap trap
=================================

Pre-v9 sparc has no compare-and-swap instruction.  A lock held inside one
process cannot make a word atomic against another process mapping the same
page, and a lock kept inside the word itself costs value bits, so the kernel
performs the operation instead.  parisc provides the same service for the same
reason.

Interface
=========

``ta 0x11`` - software trap 0x11, trap type 0x91, ``SP_TRAP_CAS`` in
``asm/traps.h`` - asks the kernel to compare and swap one 32-bit word:

=======  =========================================
``%o0``  word aligned user address
``%o1``  expected value
``%o2``  new value
=======  =========================================

On return the carry bit is clear and ``%o0`` holds the value that was found at
the address.  The caller compares it against what it expected to learn whether
the swap happened; there is no separate success flag.

On failure the carry bit is set and ``%o0`` holds an error number:

==========  =========================================
``EINVAL``  the address is not word aligned
``EFAULT``  the memory access could not be completed
==========  =========================================

``EFAULT`` is not a promise that the address was validated.  Where the kernel
performs the swap itself it writes only when the comparison succeeds, so a
mismatching value on a read-only mapping can return the value it found without
ever discovering that the mapping could not be written.

The operation is atomic against another process issuing the same trap, against
the kernel's own futex operations on that word, and - on a cpu that implements
``casa`` - against a binary using that instruction directly.

Atomicity is all it promises.  Userspace remains responsible for whatever
memory ordering the operation it is implementing requires; the trap does not
define a memory model.

What userspace has to do as well
================================

On a cpu without ``casa`` the kernel does the swap under a lock of its own, and
**an ordinary store does not take that lock**::

	trap                        another thread's plain store

	lock
	read word -> A
	                            store B -> word
	write C -> word
	unlock

``B`` is lost, and no ordering of an atomic compare-and-swap and an atomic
store makes that a legal outcome.

So userspace using this service to build atomic operations must build *every*
atomic write from it, not just compare-and-exchange: an atomic store and an
atomic exchange each become a compare-and-swap retry loop.  An implementation
that routes read-modify-writes through the trap but leaves atomic stores as
plain stores is not atomic, and the failure is silent.

This does not apply to a cpu that has ``casa``, where the kernel uses the
instruction and userspace can too.

Availability
============

``AT_HWCAP`` carries ``HWCAP_SPARC_CASTRAP`` (0x10000000) when the kernel
implements the trap.  Userspace must consult it rather than probing, because
there is no safe way to probe:

- on a kernel without this support the trap is fatal;
- on sparc64 a 32-bit process reads its hwcap word from ``asm/elf_64.h``, where
  the bit is reserved and always clear, and ``ta 0x11`` there is not an illegal
  instruction but the old 64-bit system call trap.

The bit describes the kernel service, not the hardware.  It is set whether or
not the cpu has ``casa``, because a binary built for plain v8 cannot use that
instruction even on a cpu that has it.  A binary that does have ``casa``
available - one built for LEON3 - should use it inline and ignore the bit
entirely; the kernel uses the instruction too where it exists, so the two agree
on the same word.
