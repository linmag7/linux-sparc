#!/usr/bin/env python3
"""Read-only prerequisite guard for this SPARC32 entry.S layout. Fails closed."""
import sys,re
from pathlib import Path
p=Path(sys.argv[1])/"arch/sparc/kernel/entry.S"
s=p.read_text()
for label,call in [("smp4m_ticker","smp4m_percpu_timer_interrupt"),("smp4d_ticker","smp4d_percpu_timer_interrupt"),("smpleon_ipi","leonsmp_ipi_interrupt")]:
    m=re.search(r"^"+label+r":(.*?)\bRESTORE_ALL\b",s,re.M|re.S)
    if not m: raise SystemExit("FAIL: cannot recognize "+label+"; manual review required")
    lines=[re.sub(r"\s+","",x.split("!")[0]) for x in m.group(1).splitlines()]
    try: tail=lines[lines.index("call"+call)+1:]
    except ValueError: raise SystemExit("FAIL: cannot locate callback in "+label)
    expected=["or%l0,PSR_PIL,%g2","wr%g2,PSR_ET,%psr","WRITE_PAUSE"]
    if not any(tail[i:i+3]==expected for i in range(len(tail)-2)) or "wr%l0,PSR_ET,%psr" in tail:
        raise SystemExit("FAIL: missing Imre-equivalent raised-PIL return at "+label)
print("PASS: all three return paths retain PIL; do not reapply the prerequisite")
