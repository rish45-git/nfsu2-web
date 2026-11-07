#!/usr/bin/env python3
"""A quick look at a Windows minidump of SPEED2.EXE (no debugger needed): the exception, the faulting thread's
registers, its stack's possible return addresses (named by module + offset), the ebp chain, and any memory asked for.
  python -I tools/dumpstack.py SPEED2.EXE.1234.dmp [addr:len ...]        (addr hex, len decimal/hex)
Minidump layout: MSDN MINIDUMP_HEADER / _DIRECTORY / _EXCEPTION_STREAM / _THREAD_LIST / _MEMORY(64)_LIST / _MODULE_LIST."""
import sys, struct
d = open(sys.argv[1], 'rb').read()
assert d[:4] == b'MDMP', 'not a minidump'
nstreams, dirrva = struct.unpack_from('<II', d, 8)
streams = {}
for i in range(nstreams):
    t, sz, rva = struct.unpack_from('<III', d, dirrva + 12 * i)
    streams.setdefault(t, []).append((sz, rva))
# memory ranges
ranges = []
for sz, rva in streams.get(5, []):
    n = struct.unpack_from('<I', d, rva)[0]
    for i in range(n):
        start, dsz, drva = struct.unpack_from('<QII', d, rva + 4 + 16 * i)
        ranges.append((start, dsz, drva))
for sz, rva in streams.get(9, []):
    n, base = struct.unpack_from('<QQ', d, rva)
    off = base
    for i in range(n):
        start, dsz = struct.unpack_from('<QQ', d, rva + 16 + 16 * i)
        ranges.append((start, dsz, off)); off += dsz
ranges.sort()
def mem(a, n):
    for start, dsz, drva in ranges:
        if start <= a < start + dsz:
            k = min(n, start + dsz - a)
            return d[drva + (a - start): drva + (a - start) + k]
    return b''
def u32(a):
    b = mem(a, 4)
    return struct.unpack('<I', b)[0] if len(b) == 4 else None
# modules
mods = []
for sz, rva in streams.get(4, []):
    n = struct.unpack_from('<I', d, rva)[0]
    for i in range(n):
        base, size, cks, ts, namerva = struct.unpack_from('<QIIII', d, rva + 4 + 108 * i)
        ln = struct.unpack_from('<I', d, namerva)[0]
        name = d[namerva + 4: namerva + 4 + ln].decode('utf-16-le', 'replace')
        mods.append((base, size, name.rsplit(chr(92), 1)[-1]))
mods.sort()
def modname(a):
    for base, size, name in mods:
        if base <= a < base + size: return '%s+0x%x' % (name, a - base)
    return None
print('streams:', sorted(streams), 'memory ranges:', len(ranges), 'bytes:', sum(r[1] for r in ranges))
for base, size, name in mods:
    if name.lower().startswith(('speed2', 'nfsu2', 'dinput8', 'd3d9', 'reshade')): print('module %-32s 0x%08x..0x%08x' % (name, base, base + size))
ctx = None; tid = None
for sz, rva in streams.get(6, []):
    tid, code, flags, rec, addr, npar = struct.unpack_from('<IxxxxIIQQI', d, rva)
    pars = struct.unpack_from('<' + 'Q' * 15, d, rva + 8 + 32)
    csz, crva = struct.unpack_from('<II', d, rva + 8 + 152)
    print('exception 0x%08x at 0x%08x (%s) thread %d, params %s' % (code, addr, modname(addr), tid, [hex(p) for p in pars[:npar]]))
    ctx = d[crva: crva + csz]
if ctx is None:
    print('no exception stream'); sys.exit(0)
# x86 CONTEXT
names = ['edi', 'esi', 'ebx', 'edx', 'ecx', 'eax', 'ebp', 'eip', 'cs', 'eflags', 'esp', 'ss']
regs = dict(zip(names, struct.unpack_from('<12I', ctx, 0x9c)))
print('regs: ' + ' '.join('%s=%08x' % (k, regs[k]) for k in names if k not in ('cs', 'ss')))
for k in ('eax', 'ebx', 'ecx', 'edx', 'esi', 'edi', 'ebp'):
    b = mem(regs[k], 64)
    if b: print('  [%s] %s' % (k, ' '.join('%08x' % v for v in struct.unpack('<%dI' % (len(b) // 4), b[:len(b) // 4 * 4]))))
# stack scan
esp = regs['esp']
print('stack from esp=0x%08x (return addresses by module):' % esp)
for i in range(0, 0x3000, 4):
    v = u32(esp + i)
    if v is None: break
    m = modname(v)
    if m and ('speed2' in m.lower() or 'nfsu2' in m.lower()) and (not m.lower().startswith('speed2') or 0x401000 <= v < 0x7e0000):
        # a call site: the 5 bytes before it a call?
        pre = mem(v - 5, 5)
        tag = ''
        if len(pre) == 5 and pre[0] == 0xe8:
            tgt = (v + struct.unpack('<i', pre[1:])[0]) & 0xffffffff
            tag = ' <- call 0x%08x' % tgt
        elif len(pre) >= 2 and pre[-2] == 0xff: tag = ' <- call [reg/mem]'
        elif len(pre) >= 2 and pre[-2] == 0xd0 and pre[-3] == 0xff: tag = ' <- call reg'
        if tag or not m.lower().startswith('speed2'): print('  esp+%04x: %08x %s%s' % (i, v, m, tag))
print('ebp chain:')
ebp = regs['ebp']
for n in range(20):
    nxt, ret = u32(ebp), u32(ebp + 4)
    if nxt is None or ret is None: break
    print('  ebp=%08x ret=%08x %s' % (ebp, ret, modname(ret)))
    if nxt <= ebp or nxt - ebp > 0x100000: break
    ebp = nxt
for spec in sys.argv[2:]:
    a, n = spec.split(':'); a = int(a, 16); n = int(n, 0)
    b = mem(a, n)
    print('memory 0x%08x (%d of %d bytes):' % (a, len(b), n))
    for o in range(0, len(b), 32):
        row = b[o:o + 32]
        print('  %08x: %s' % (a + o, ' '.join('%08x' % v for v in struct.unpack('<%dI' % (len(row) // 4), row[:len(row) // 4 * 4]))))
