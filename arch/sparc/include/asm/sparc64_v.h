/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SPARC64_V_H
#define _SPARC64_V_H

/* SPARC64 V Supplement R1.0, F.10.1 Table F-3, pp92-93. */
#define SPARC64_V_MCNTL_NC_CACHE	(1UL << 16)
#define SPARC64_V_MCNTL_FORCE_I	(1UL << 15)
#define SPARC64_V_MCNTL_FORCE_D	(1UL << 14)
#define SPARC64_V_MCNTL_RMD_MASK	(3UL << 12)
#define SPARC64_V_MCNTL_RMD_2	(2UL << 12)
#define SPARC64_V_MCNTL_HASH	(1UL << 8)
#define SPARC64_V_MCNTL_UNSUPPORTED (SPARC64_V_MCNTL_NC_CACHE | \
	SPARC64_V_MCNTL_FORCE_I | SPARC64_V_MCNTL_FORCE_D | \
	SPARC64_V_MCNTL_HASH)

/* V 5.2.12 Table 5-3, and R.3 Table R-3. */
#define SPARC64_V_DCU_MMU_ENABLE	0xcUL
#define SPARC64_V_DCU_WEAK_SPCA	(1UL << 41)
#define SPARC64_V_UPA_AM_SHIFT	39
#define SPARC64_V_UPA_MID_SHIFT	17

unsigned long sparc64_v_secondary_check(void);

#endif /* _SPARC64_V_H */
