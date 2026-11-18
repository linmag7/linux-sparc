#!/usr/bin/env python3
"""Fail-closed, read-only checks for the reviewed SPARC32 entry.S layout."""
import re, sys
from pathlib import Path

def verify(s):
    for label, call in [('smp4m_ticker','smp4m_percpu_timer_interrupt'),
                        ('smp4d_ticker','smp4d_percpu_timer_interrupt'),
                        ('smpleon_ipi','leonsmp_ipi_interrupt')]:
        m=re.search(r'^'+label+r':(.*?)\bRESTORE_ALL\b',s,re.M|re.S)
        if not m: raise ValueError('unrecognized '+label+'; manual review required')
        lines=[re.sub(r'\s+','',x.split('!')[0]) for x in m.group(1).splitlines()]
        try: tail=lines[lines.index('call'+call)+1:]
        except ValueError: raise ValueError('missing callback '+label)
        required=['or%l0,PSR_PIL,%g2','wr%g2,PSR_ET,%psr','WRITE_PAUSE']
        if not any(tail[i:i+3]==required for i in range(len(tail)-2)) or 'wr%l0,PSR_ET,%psr' in tail:
            raise ValueError('missing Imre raised-PIL exit at '+label)
    m=re.search(r'^maybe_smp4m_msg:(.*?)^maybe_smp4m_msg_check_single:',s,re.M|re.S)
    if not m: raise ValueError('unrecognized sun4m IPI handler; manual review required')
    block=re.sub(r'/\*.*?\*/','',m.group(1),flags=re.S)
    lines=[re.sub(r'\s+','',x.split('!')[0]) for x in block.splitlines()]
    lines=[x for x in lines if x]
    seq=['ld[%o1+0x00],%o3','andcc%o3,%o2,%o4','be,asmp4m_ticker','cmp%l7,14','st%o4,[%o1+0x04]']
    if sum(lines[i:i+5]==seq for i in range(len(lines)-4))!=1:
        raise ValueError('missing observed-IPI acknowledgement sequence')
    if [x for x in lines if x.startswith('st')]!=['st%o4,[%o1+0x04]']:
        raise ValueError('unexpected additional store in IPI acknowledgement')
    if lines.count('sethi%hi(0x70000000),%o2')!=1 or lines[-1]!='srl%o3,28,%o2':
        raise ValueError('unrecognized snapshot/mask dispatch; manual review required')
    # Only these instructions may write the snapshot between its read and use.
    mid=lines[lines.index(seq[0])+1:-1]
    if any(x.endswith(',%o3') for x in mid):
        raise ValueError('snapshot clobbered before dispatch')
    return 'PASS: Imre raised-PIL exits and Ramin observed-IPI acknowledgement present'

if __name__=='__main__':
    try: print(verify((Path(sys.argv[1])/'arch/sparc/kernel/entry.S').read_text()))
    except (ValueError, OSError, IndexError) as e: raise SystemExit('FAIL: '+str(e))
