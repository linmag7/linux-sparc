#!/usr/bin/env python3
"""Read-only full-stack prerequisite gate; unknown source needs manual review."""
from pathlib import Path
import re, subprocess, sys
ROOT = Path(__file__).resolve().parent.parent

def verify_protnone(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    match = re.search(r'static\s+inline\s+int\s+pte_present\s*\(pte_t\s+pte\)\s*\{([^{}]*)\}', text)
    expected = 'return(pte_val(pte)&SRMMU_ET_MASK)==SRMMU_ET_PTE||(pte_val(pte)&SRMMU_PRIV_RDONLY)==SRMMU_PRIV_RDONLY;'
    if not match or re.sub(r'\s+', '', match[1]) != expected:
        raise ValueError('PROT_NONE resident/COW predicate absent or changed; manual equivalent-code review required')

if __name__ == '__main__':
    try:
        source = Path(sys.argv[1]).resolve()
        subprocess.run([sys.executable, str(ROOT/'sparc32-build-prerequisites-20261018/verify-sparc32-prerequisites.py'), str(source)], check=True)
        verify_protnone((source/'arch/sparc/include/asm/pgtable_32.h').read_text())
        print('PASS: PROT_NONE resident/COW predicate present; all three external prerequisites verified')
    except (IndexError, OSError, ValueError, subprocess.CalledProcessError) as exc:
        raise SystemExit('FAIL: '+str(exc))
